/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "mutation_log.h"
#include "replica.h"
#include "mutation_log_utils.h"

#include <dsn/utility/filesystem.h>
#include <dsn/utility/crc.h>
#include <dsn/utility/fail_point.h>
#include <dsn/dist/fmt_logging.h>
#include <dsn/tool-api/async_calls.h>

namespace dsn {
namespace replication {

::dsn::task_ptr mutation_log_shared::append(mutation_ptr &mu,
                                            dsn::task_code callback_code,
                                            dsn::task_tracker *tracker,
                                            aio_handler &&callback,
                                            int hash,
                                            int64_t *pending_size)
{
    auto d = mu->data.header.decree;
    ::dsn::aio_task_ptr cb =
        callback ? file::create_aio_task(
                       callback_code, tracker, std::forward<aio_handler>(callback), hash)
                 : nullptr;

    _slock.lock();

    // init pending buffer
    if (nullptr == _pending_write) {
        _pending_write = std::make_shared<log_appender>(mark_new_offset(0, true).second);
    }
    _pending_write->append_mutation(mu, cb);

    // update meta
    update_max_decree(mu->data.header.pid, d);

    // start to write if possible
    if (!_is_writing.load(std::memory_order_acquire)) {
        write_pending_mutations(true);
        if (pending_size) {
            *pending_size = 0;
        }
    } else {
        if (pending_size) {
            *pending_size = _pending_write->size();
        }
        _slock.unlock();
    }
    return cb;
}

void mutation_log_shared::flush() { flush_internal(-1); }

void mutation_log_shared::flush_once() { flush_internal(1); }

void mutation_log_shared::flush_internal(int max_count)
{
    int count = 0;
    while (max_count <= 0 || count < max_count) {
        if (_is_writing.load(std::memory_order_acquire)) {
            _tracker.wait_outstanding_tasks();
        } else {
            _slock.lock();
            if (_is_writing.load(std::memory_order_acquire)) {
                _slock.unlock();
                continue;
            }
            if (!_pending_write) {
                // !_is_writing && !_pending_write, means flush done
                _slock.unlock();
                break;
            }
            // !_is_writing && _pending_write, start next write
            write_pending_mutations(true);
            count++;
        }
    }
}

void mutation_log_shared::write_pending_mutations(bool release_lock_required)
{
    dassert(release_lock_required, "lock must be hold at this point");
    dassert(!_is_writing.load(std::memory_order_relaxed), "");
    dassert(_pending_write != nullptr, "");
    dassert(_pending_write->size() > 0, "pending write size = %d", (int)_pending_write->size());
    auto pr = mark_new_offset(_pending_write->size(), false);
    dcheck_eq(pr.second, _pending_write->start_offset());

    _is_writing.store(true, std::memory_order_release);

    // move or reset pending variables
    auto pending = std::move(_pending_write);

    // seperate commit_log_block from within the lock
    _slock.unlock();
    commit_pending_mutations(pr.first, pending);
}

void mutation_log_shared::commit_pending_mutations(log_file_ptr &lf,
                                                   std::shared_ptr<log_appender> &pending)
{
    lf->commit_log_blocks( // forces a new line for params
        *pending,
        LPC_WRITE_REPLICATION_LOG_SHARED,
        &_tracker,
        [this, lf, pending](error_code err, size_t sz) mutable {
            dassert(_is_writing.load(std::memory_order_relaxed), "");

            for (auto &block : pending->all_blocks()) {
                auto hdr = (log_block_header *)block.front().data();
                dassert(hdr->magic == 0xdeadbeef, "header magic is changed: 0x%x", hdr->magic);
            }

            if (err == ERR_OK) {
                dcheck_eq(sz, pending->size());

                if (_force_flush) {
                    // flush to ensure that shared log data synced to disk
                    //
                    // FIXME : the file could have been closed
                    lf->flush();
                }

                if (_write_size_counter) {
                    (*_write_size_counter)->add(sz);
                }
            } else {
                derror("write shared log failed, err = %s", err.to_string());
            }

            // here we use _is_writing instead of _issued_write.expired() to check writing done,
            // because the following callbacks may run before "block" released, which may cause
            // the next init_prepare() not starting the write.
            _is_writing.store(false, std::memory_order_relaxed);

            // notify the callbacks
            // ATTENTION: callback may be called before this code block executed done.
            for (auto &c : pending->callbacks()) {
                c->enqueue(err, sz);
            }

            // start to write next if possible
            if (err == ERR_OK) {
                _slock.lock();

                if (!_is_writing.load(std::memory_order_acquire) && _pending_write) {
                    write_pending_mutations(true);
                } else {
                    _slock.unlock();
                }
            }
        },
        0);
}

////////////////////////////////////////////////////

mutation_log_private::mutation_log_private(const std::string &dir,
                                           int32_t max_log_file_mb,
                                           gpid gpid,
                                           replica *r,
                                           uint32_t batch_buffer_bytes,
                                           uint32_t batch_buffer_max_count,
                                           uint64_t batch_buffer_flush_interval_ms)
    : mutation_log(dir, max_log_file_mb, gpid, r),
      replica_base(r),
      _batch_buffer_bytes(batch_buffer_bytes),
      _batch_buffer_max_count(batch_buffer_max_count),
      _batch_buffer_flush_interval_ms(batch_buffer_flush_interval_ms)
{
    mutation_log_private::init_states();
}

::dsn::task_ptr mutation_log_private::append(mutation_ptr &mu,
                                             dsn::task_code callback_code,
                                             dsn::task_tracker *tracker,
                                             aio_handler &&callback,
                                             int hash,
                                             int64_t *pending_size)
{
    dassert(nullptr == callback, "callback is not needed in private mutation log");

    _plock.lock();

    // init pending buffer
    if (nullptr == _pending_write) {
        _pending_write = make_unique<log_appender>(mark_new_offset(0, true).second);
        _pending_write_start_time_ms = dsn_now_ms();
    }
    _pending_write->append_mutation(mu, nullptr);

    // update meta
    _pending_write_max_commit =
        std::max(_pending_write_max_commit, mu->data.header.last_committed_decree);
    _pending_write_max_decree = std::max(_pending_write_max_decree, mu->data.header.decree);

    // start to write if possible
    if (!_is_writing.load(std::memory_order_acquire) &&
        (static_cast<uint32_t>(_pending_write->size()) >= _batch_buffer_bytes ||
         static_cast<uint32_t>(_pending_write->blob_count()) >= _batch_buffer_max_count ||
         flush_interval_expired())) {
        write_pending_mutations(true);
        if (pending_size) {
            *pending_size = 0;
        }
    } else {
        if (pending_size) {
            *pending_size = _pending_write->size();
        }
        _plock.unlock();
    }

    return nullptr;
}

bool mutation_log_private::get_learn_state_in_memory(decree start_decree,
                                                     binary_writer &writer) const
{
    std::shared_ptr<log_appender> issued_write;
    mutations pending_mutations;
    {
        zauto_lock l(_plock);

        issued_write = _issued_write.lock();

        if (_pending_write) {
            pending_mutations = _pending_write->mutations();
        }
    }

    int learned_count = 0;

    if (issued_write) {
        for (auto &mu : issued_write->mutations()) {
            if (mu->get_decree() >= start_decree) {
                mu->write_to(writer, nullptr);
                learned_count++;
            }
        }
    }

    for (auto &mu : pending_mutations) {
        if (mu->get_decree() >= start_decree) {
            mu->write_to(writer, nullptr);
            learned_count++;
        }
    }

    return learned_count > 0;
}

void mutation_log_private::get_in_memory_mutations(decree start_decree,
                                                   ballot start_ballot,
                                                   std::vector<mutation_ptr> &mutation_list) const
{
    std::shared_ptr<log_appender> issued_write;
    mutations pending_mutations;
    {
        zauto_lock l(_plock);
        issued_write = _issued_write.lock();
        if (_pending_write) {
            pending_mutations = _pending_write->mutations();
        }
    }

    if (issued_write) {
        for (auto &mu : issued_write->mutations()) {
            // if start_ballot is invalid or equal to mu.ballot, check decree
            // otherwise check ballot
            ballot current_ballot =
                (start_ballot == invalid_ballot) ? invalid_ballot : mu->get_ballot();
            if ((mu->get_decree() >= start_decree && start_ballot == current_ballot) ||
                current_ballot > start_ballot) {
                mutation_list.push_back(mutation::copy_no_reply(mu));
            }
        }
    }

    for (auto &mu : pending_mutations) {
        // if start_ballot is invalid or equal to mu.ballot, check decree
        // otherwise check ballot
        ballot current_ballot =
            (start_ballot == invalid_ballot) ? invalid_ballot : mu->get_ballot();
        if ((mu->get_decree() >= start_decree && start_ballot == current_ballot) ||
            current_ballot > start_ballot) {
            mutation_list.push_back(mutation::copy_no_reply(mu));
        }
    }
}

void mutation_log_private::flush() { flush_internal(-1); }

void mutation_log_private::flush_once() { flush_internal(1); }

void mutation_log_private::flush_internal(int max_count)
{
    int count = 0;
    while (max_count <= 0 || count < max_count) {
        if (_is_writing.load(std::memory_order_acquire)) {
            _tracker.wait_outstanding_tasks();
        } else {
            _plock.lock();
            if (_is_writing.load(std::memory_order_acquire)) {
                _plock.unlock();
                continue;
            }
            if (!_pending_write) {
                // !_is_writing && !_pending_write, means flush done
                _plock.unlock();
                break;
            }
            // !_is_writing && _pending_write, start next write
            write_pending_mutations(true);
            count++;
        }
    }
}

void mutation_log_private::init_states()
{
    mutation_log::init_states();

    _is_writing.store(false, std::memory_order_release);
    _issued_write.reset();
    _pending_write = nullptr;
    _pending_write_start_time_ms = 0;
    _pending_write_max_commit = 0;
    _pending_write_max_decree = 0;
}

void mutation_log_private::write_pending_mutations(bool release_lock_required)
{
    dassert(release_lock_required, "lock must be hold at this point");
    dassert(!_is_writing.load(std::memory_order_relaxed), "");
    dassert(_pending_write != nullptr, "");
    dassert(_pending_write->size() > 0, "pending write size = %d", (int)_pending_write->size());
    auto pr = mark_new_offset(_pending_write->size(), false);
    dcheck_eq_replica(pr.second, _pending_write->start_offset());

    _is_writing.store(true, std::memory_order_release);

    update_max_decree(_private_gpid, _pending_write_max_decree);

    // move or reset pending variables
    std::shared_ptr<log_appender> pending = std::move(_pending_write);
    _issued_write = pending;
    _pending_write_start_time_ms = 0;
    decree max_commit = _pending_write_max_commit;
    _pending_write_max_commit = 0;
    _pending_write_max_decree = 0;

    // Free plog from lock during committing log block, in the meantime
    // new mutations can still be appended.
    _plock.unlock();
    commit_pending_mutations(pr.first, pending, max_commit);
}

void mutation_log_private::commit_pending_mutations(log_file_ptr &lf,
                                                    std::shared_ptr<log_appender> &pending,
                                                    decree max_commit)
{
    lf->commit_log_blocks(
        *pending,
        LPC_WRITE_REPLICATION_LOG_PRIVATE,
        &_tracker,
        [this, lf, pending, max_commit](error_code err, size_t sz) mutable {
            dassert(_is_writing.load(std::memory_order_relaxed), "");

            for (auto &block : pending->all_blocks()) {
                auto hdr = (log_block_header *)block.front().data();
                dassert(hdr->magic == 0xdeadbeef, "header magic is changed: 0x%x", hdr->magic);
            }

            if (err != ERR_OK) {
                derror("write private log failed, err = %s", err.to_string());
                _is_writing.store(false, std::memory_order_relaxed);
                if (_io_error_callback) {
                    _io_error_callback(err);
                }
                return;
            }
            dcheck_eq(sz, pending->size());

            // flush to ensure that there is no gap between private log and in-memory buffer
            // so that we can get all mutations in learning process.
            //
            // FIXME : the file could have been closed
            lf->flush();

            // update _private_max_commit_on_disk after written into log file done
            update_max_commit_on_disk(max_commit);

            _is_writing.store(false, std::memory_order_relaxed);

            // start to write if possible
            _plock.lock();

            if (!_is_writing.load(std::memory_order_acquire) && _pending_write &&
                (static_cast<uint32_t>(_pending_write->size()) >= _batch_buffer_bytes ||
                 static_cast<uint32_t>(_pending_write->blob_count()) >= _batch_buffer_max_count ||
                 flush_interval_expired())) {
                write_pending_mutations(true);
            } else {
                _plock.unlock();
            }
        },
        0);
}

///////////////////////////////////////////////////////////////

mutation_log::mutation_log(const std::string &dir, int32_t max_log_file_mb, gpid gpid, replica *r)
{
    _dir = dir;
    _is_private = (gpid.value() != 0);
    _max_log_file_size_in_bytes = static_cast<int64_t>(max_log_file_mb) * 1024L * 1024L;
    _min_log_file_size_in_bytes = _max_log_file_size_in_bytes / 10;
    _owner_replica = r;
    _private_gpid = gpid;

    if (r) {
        dassert(_private_gpid == r->get_gpid(),
                "(%d.%d) VS (%d.%d)",
                _private_gpid.get_app_id(),
                _private_gpid.get_partition_index(),
                r->get_gpid().get_app_id(),
                r->get_gpid().get_partition_index());
    }
    mutation_log::init_states();
}

void mutation_log::init_states()
{
    _is_opened = false;
    _switch_file_hint = false;
    _switch_file_demand = false;

    // logs
    _last_file_index = 0;
    _log_files.clear();
    _current_log_file = nullptr;
    _global_start_offset = 0;
    _global_end_offset = 0;

    // replica states
    _shared_log_info_map.clear();
    _private_log_info = {0, 0};
    _private_max_commit_on_disk = 0;
}

error_code mutation_log::open(replay_callback read_callback,
                              io_failure_callback write_error_callback)
{
    std::map<gpid, decree> replay_condition;
    return open(read_callback, write_error_callback, replay_condition);
}

error_code mutation_log::open(replay_callback read_callback,
                              io_failure_callback write_error_callback,
                              const std::map<gpid, decree> &replay_condition)
{
    dassert(!_is_opened, "cannot open a opened mutation_log");
    dassert(nullptr == _current_log_file, "the current log file must be null at this point");

    // create dir if necessary
    if (!dsn::utils::filesystem::path_exists(_dir)) {
        if (!dsn::utils::filesystem::create_directory(_dir)) {
            derror("open mutation_log: create log path failed");
            return ERR_FILE_OPERATION_FAILED;
        }
    }

    // load the existing logs
    _log_files.clear();
    _io_error_callback = write_error_callback;

    std::vector<std::string> file_list;
    if (!dsn::utils::filesystem::get_subfiles(_dir, file_list, false)) {
        derror("open mutation_log: get subfiles failed.");
        return ERR_FILE_OPERATION_FAILED;
    }

    if (nullptr == read_callback) {
        dassert(file_list.size() == 0, "log must be empty if callback is not present");
    }

    std::sort(file_list.begin(), file_list.end());

    error_code err = ERR_OK;
    for (auto &fpath : file_list) {
        log_file_ptr log = log_file::open_read(fpath.c_str(), err);
        if (log == nullptr) {
            if (err == ERR_HANDLE_EOF || err == ERR_INCOMPLETE_DATA ||
                err == ERR_INVALID_PARAMETERS) {
                dwarn("skip file %s during log init, err = %s", fpath.c_str(), err.to_string());
                continue;
            } else {
                return err;
            }
        }

        if (_is_private) {
            ddebug("open private log %s succeed, start_offset = %" PRId64 ", end_offset = %" PRId64
                   ", size = %" PRId64 ", previous_max_decree = %" PRId64,
                   fpath.c_str(),
                   log->start_offset(),
                   log->end_offset(),
                   log->end_offset() - log->start_offset(),
                   log->previous_log_max_decree(_private_gpid));
        } else {
            ddebug("open shared log %s succeed, start_offset = %" PRId64 ", end_offset = %" PRId64
                   ", size = %" PRId64 "",
                   fpath.c_str(),
                   log->start_offset(),
                   log->end_offset(),
                   log->end_offset() - log->start_offset());
        }

        dassert(_log_files.find(log->index()) == _log_files.end(),
                "invalid log_index, index = %d",
                log->index());
        _log_files[log->index()] = log;
    }

    file_list.clear();

    // filter useless log
    std::map<int, log_file_ptr>::iterator replay_begin = _log_files.begin();
    std::map<int, log_file_ptr>::iterator replay_end = _log_files.end();
    if (!replay_condition.empty()) {
        if (_is_private) {
            auto find = replay_condition.find(_private_gpid);
            dassert(find != replay_condition.end(),
                    "invalid gpid(%d.%d)",
                    _private_gpid.get_app_id(),
                    _private_gpid.get_partition_index());
            for (auto it = _log_files.begin(); it != _log_files.end(); ++it) {
                if (it->second->previous_log_max_decree(_private_gpid) <= find->second) {
                    // previous logs can be ignored
                    replay_begin = it;
                } else {
                    break;
                }
            }
        } else {
            // find the largest file which can be ignored.
            // after iterate, the 'mark_it' will point to the largest file which can be ignored.
            std::map<int, log_file_ptr>::reverse_iterator mark_it;
            std::set<gpid> kickout_replicas;
            replica_log_info_map max_decrees; // max_decrees for log file at mark_it.
            for (mark_it = _log_files.rbegin(); mark_it != _log_files.rend(); ++mark_it) {
                bool ignore_this = true;

                if (mark_it == _log_files.rbegin()) {
                    // the last file should not be ignored
                    ignore_this = false;
                }

                if (ignore_this) {
                    for (auto &kv : replay_condition) {
                        if (kickout_replicas.find(kv.first) != kickout_replicas.end()) {
                            // no need to consider this replica
                            continue;
                        }

                        auto find = max_decrees.find(kv.first);
                        if (find == max_decrees.end() || find->second.max_decree <= kv.second) {
                            // can ignore for this replica
                            kickout_replicas.insert(kv.first);
                        } else {
                            ignore_this = false;
                            break;
                        }
                    }
                }

                if (ignore_this) {
                    // found the largest file which can be ignored
                    break;
                }

                // update max_decrees for the next log file
                max_decrees = mark_it->second->previous_log_max_decrees();
            }

            if (mark_it != _log_files.rend()) {
                // set replay_begin to the next position of mark_it.
                replay_begin = _log_files.find(mark_it->first);
                dassert(replay_begin != _log_files.end(),
                        "invalid log_index, index = %d",
                        mark_it->first);
                replay_begin++;
                dassert(replay_begin != _log_files.end(),
                        "invalid log_index, index = %d",
                        mark_it->first);
            }
        }

        for (auto it = _log_files.begin(); it != replay_begin; it++) {
            ddebug("ignore log %s", it->second->path().c_str());
        }
    }

    // replay with the found files
    std::map<int, log_file_ptr> replay_logs(replay_begin, replay_end);
    int64_t end_offset = 0;
    err = replay(
        replay_logs,
        [this, read_callback](int log_length, mutation_ptr &mu) {
            bool ret = true;

            if (read_callback) {
                ret = read_callback(log_length,
                                    mu); // actually replica::replay_mutation(mu, true|false);
            }

            if (ret) {
                this->update_max_decree_no_lock(mu->data.header.pid, mu->data.header.decree);
                if (this->_is_private) {
                    this->update_max_commit_on_disk_no_lock(mu->data.header.last_committed_decree);
                }
            }

            return ret;
        },
        end_offset);

    if (ERR_OK == err) {
        _global_start_offset =
            _log_files.size() > 0 ? _log_files.begin()->second->start_offset() : 0;
        _global_end_offset = end_offset;
        _last_file_index = _log_files.size() > 0 ? _log_files.rbegin()->first : 0;
        _is_opened = true;
    } else {
        // clear
        for (auto &kv : _log_files) {
            kv.second->close();
        }
        _log_files.clear();
        init_states();
    }

    return err;
}

void mutation_log::close()
{
    {
        zauto_lock l(_lock);
        if (!_is_opened) {
            return;
        }
        _is_opened = false;
    }

    dinfo("close mutation log %s", dir().c_str());

    // make all data is on disk
    flush();

    {
        zauto_lock l(_lock);

        // close current log file
        if (nullptr != _current_log_file) {
            _current_log_file->close();
            _current_log_file = nullptr;
        }
    }

    // reset all states
    init_states();
}

error_code mutation_log::create_new_log_file()
{
    // create file
    uint64_t start = dsn_now_ns();
    log_file_ptr logf =
        log_file::create_write(_dir.c_str(), _last_file_index + 1, _global_end_offset);
    if (logf == nullptr) {
        derror("cannot create log file with index %d", _last_file_index + 1);
        return ERR_FILE_OPERATION_FAILED;
    }
    dassert(logf->end_offset() == logf->start_offset(),
            "%" PRId64 " VS %" PRId64 "",
            logf->end_offset(),
            logf->start_offset());
    dassert(_global_end_offset == logf->end_offset(),
            "%" PRId64 " VS %" PRId64 "",
            _global_end_offset,
            logf->start_offset());
    ddebug("create new log file %s succeed, time_used = %" PRIu64 " ns",
           logf->path().c_str(),
           dsn_now_ns() - start);

    // update states
    _last_file_index++;
    dassert(_log_files.find(_last_file_index) == _log_files.end(),
            "invalid log_offset, offset = %d",
            _last_file_index);
    _log_files[_last_file_index] = logf;

    // switch the current log file
    // the old log file may be hold by _log_files or aio_task
    _current_log_file = logf;

    // create new pending buffer because we need write file header
    // write file header into pending buffer
    size_t header_len = 0;
    binary_writer temp_writer;
    if (_is_private) {
        replica_log_info_map ds;
        ds[_private_gpid] =
            replica_log_info(_private_log_info.max_decree, _private_log_info.valid_start_offset);
        header_len = logf->write_file_header(temp_writer, ds);
    } else {
        header_len = logf->write_file_header(temp_writer, _shared_log_info_map);
    }

    log_block *blk = new log_block();
    blk->add(temp_writer.get_buffer());
    _global_end_offset += blk->size();

    logf->commit_log_block(*blk,
                           _current_log_file->start_offset(),
                           LPC_WRITE_REPLICATION_LOG_COMMON,
                           &_tracker,
                           [this, blk, logf](::dsn::error_code err, size_t sz) {
                               delete blk;
                               if (ERR_OK != err) {
                                   derror(
                                       "write mutation log file header failed, file = %s, err = %s",
                                       logf->path().c_str(),
                                       err.to_string());
                                   if (_io_error_callback) {
                                       _io_error_callback(err);
                                   } else {
                                       dassert(false, "unhandled error");
                                   }
                               }
                           },
                           0);

    dassert(_global_end_offset ==
                _current_log_file->start_offset() + sizeof(log_block_header) + header_len,
            "%" PRId64 " VS %" PRId64 "(%" PRId64 " + %d + %d)",
            _global_end_offset,
            _current_log_file->start_offset() + sizeof(log_block_header) + header_len,
            _current_log_file->start_offset(),
            (int)sizeof(log_block_header),
            (int)header_len);
    return ERR_OK;
}

std::pair<log_file_ptr, int64_t> mutation_log::mark_new_offset(size_t size,
                                                               bool create_new_log_if_needed)
{
    zauto_lock l(_lock);

    if (create_new_log_if_needed) {
        bool create_file = false;
        if (_current_log_file == nullptr) {
            create_file = true;
        } else {
            int64_t file_size = _global_end_offset - _current_log_file->start_offset();
            const char *reason = nullptr;

            if (_switch_file_demand) {
                create_file = true;
                reason = "demand";
            } else if (file_size >= _max_log_file_size_in_bytes) {
                create_file = true;
                reason = "limit";
            } else if (_switch_file_hint && file_size >= _min_log_file_size_in_bytes) {
                create_file = true;
                reason = "hint";
            }

            if (create_file) {
                ddebug("switch log file by %s, old_file = %s, size = %" PRId64,
                       reason,
                       _current_log_file->path().c_str(),
                       file_size);
            }
        }

        if (create_file) {
            auto ec = create_new_log_file();
            dassert_f(ec == ERR_OK,
                      "{} create new log file failed: {}",
                      _is_private ? _private_gpid.to_string() : "",
                      ec);
            _switch_file_hint = false;
            _switch_file_demand = false;
        }
    } else {
        dassert(_current_log_file != nullptr, "");
    }

    int64_t write_start_offset = _global_end_offset;
    _global_end_offset += size;

    return std::make_pair(_current_log_file, write_start_offset);
}

decree mutation_log::max_decree(gpid gpid) const
{
    zauto_lock l(_lock);
    if (_is_private) {
        dassert(gpid == _private_gpid, "replica gpid does not match");
        return _private_log_info.max_decree;
    } else {
        auto it = _shared_log_info_map.find(gpid);
        if (it != _shared_log_info_map.end())
            return it->second.max_decree;
        else
            return 0;
    }
}

decree mutation_log::max_commit_on_disk() const
{
    zauto_lock l(_lock);
    dassert(_is_private, "this method is only valid for private logs");
    return _private_max_commit_on_disk;
}

decree mutation_log::max_gced_decree(gpid gpid) const
{
    zauto_lock l(_lock);
    return max_gced_decree_no_lock(gpid);
}

decree mutation_log::max_gced_decree_no_lock(gpid gpid) const
{
    dassert(_is_private, "");

    decree result = invalid_decree;
    for (auto &log : _log_files) {
        auto it = log.second->previous_log_max_decrees().find(gpid);
        if (it != log.second->previous_log_max_decrees().end()) {
            if (result == invalid_decree) {
                result = it->second.max_decree;
            } else {
                result = std::min(result, it->second.max_decree);
            }
        }
    }
    return result;
}

void mutation_log::check_valid_start_offset(gpid gpid, int64_t valid_start_offset) const
{
    zauto_lock l(_lock);
    if (_is_private) {
        dassert(valid_start_offset == _private_log_info.valid_start_offset,
                "valid start offset mismatch: %" PRId64 " vs %" PRId64,
                valid_start_offset,
                _private_log_info.valid_start_offset);
    } else {
        auto it = _shared_log_info_map.find(gpid);
        if (it != _shared_log_info_map.end()) {
            dassert(valid_start_offset == it->second.valid_start_offset,
                    "valid start offset mismatch: %" PRId64 " vs %" PRId64,
                    valid_start_offset,
                    it->second.valid_start_offset);
        }
    }
}

int64_t mutation_log::total_size() const
{
    zauto_lock l(_lock);
    return total_size_no_lock();
}

int64_t mutation_log::total_size_no_lock() const
{
    return _log_files.size() > 0 ? _global_end_offset - _global_start_offset : 0;
}

void mutation_log::set_valid_start_offset_on_open(gpid gpid, int64_t valid_start_offset)
{
    zauto_lock l(_lock);
    if (_is_private) {
        dassert(gpid == _private_gpid, "replica gpid does not match");
        _private_log_info.valid_start_offset = valid_start_offset;
    } else {
        _shared_log_info_map[gpid] = replica_log_info(0, valid_start_offset);
    }
}

int64_t mutation_log::on_partition_reset(gpid gpid, decree max_decree)
{
    zauto_lock l(_lock);
    if (_is_private) {
        dassert(_private_gpid == gpid, "replica gpid does not match");
        replica_log_info old_info = _private_log_info;
        _private_log_info.max_decree = max_decree;
        _private_log_info.valid_start_offset = _global_end_offset;
        dwarn("replica %d.%d has changed private log max_decree from %" PRId64 " to %" PRId64
              ", valid_start_offset from %" PRId64 " to %" PRId64,
              gpid.get_app_id(),
              gpid.get_partition_index(),
              old_info.max_decree,
              _private_log_info.max_decree,
              old_info.valid_start_offset,
              _private_log_info.valid_start_offset);
    } else {
        replica_log_info info(max_decree, _global_end_offset);
        auto it = _shared_log_info_map.insert(replica_log_info_map::value_type(gpid, info));
        if (!it.second) {
            dwarn("replica %d.%d has changed shared log max_decree from %" PRId64 " to %" PRId64
                  ", valid_start_offset from %" PRId64 " to %" PRId64,
                  gpid.get_app_id(),
                  gpid.get_partition_index(),
                  it.first->second.max_decree,
                  info.max_decree,
                  it.first->second.valid_start_offset,
                  info.valid_start_offset);
            _shared_log_info_map[gpid] = info;
        }
    }
    return _global_end_offset;
}

void mutation_log::on_partition_removed(gpid gpid)
{
    dassert(!_is_private, "this method is only valid for shared logs");
    zauto_lock l(_lock);
    _shared_log_info_map.erase(gpid);
}

void mutation_log::update_max_decree(gpid gpid, decree d)
{
    zauto_lock l(_lock);
    update_max_decree_no_lock(gpid, d);
}

void mutation_log::update_max_decree_no_lock(gpid gpid, decree d)
{
    if (!_is_private) {
        auto it = _shared_log_info_map.find(gpid);
        if (it != _shared_log_info_map.end()) {
            if (it->second.max_decree < d) {
                it->second.max_decree = d;
            }
        } else {
            dassert(false, "replica has not been registered in the log before");
        }
    } else {
        dcheck_eq(gpid, _private_gpid);
        if (d > _private_log_info.max_decree) {
            _private_log_info.max_decree = d;
        }
    }
}

void mutation_log::update_max_commit_on_disk(decree d)
{
    zauto_lock l(_lock);
    update_max_commit_on_disk_no_lock(d);
}

void mutation_log::update_max_commit_on_disk_no_lock(decree d)
{
    dassert(_is_private, "this method is only valid for private logs");
    if (d > _private_max_commit_on_disk) {
        _private_max_commit_on_disk = d;
    }
}

bool mutation_log::get_learn_state(gpid gpid, decree start, /*out*/ learn_state &state) const
{
    dassert(_is_private, "this method is only valid for private logs");
    dassert(_private_gpid == gpid,
            "replica gpid does not match, (%d.%d) VS (%d.%d)",
            _private_gpid.get_app_id(),
            _private_gpid.get_partition_index(),
            gpid.get_app_id(),
            gpid.get_partition_index());

    binary_writer temp_writer;
    if (get_learn_state_in_memory(start, temp_writer)) {
        state.meta = temp_writer.get_buffer();
    }

    std::map<int, log_file_ptr> files;
    {
        zauto_lock l(_lock);

        if (state.meta.length() == 0 && start > _private_log_info.max_decree) {
            // no memory data and no disk data
            ddebug_f("gpid({}) get_learn_state returns false"
                     "learn_start_decree={}, max_decree_in_private_log={}",
                     gpid,
                     start,
                     _private_log_info.max_decree);
            return false;
        }

        files = _log_files;
    }

    // find all applicable files
    bool skip_next = false;
    std::list<std::string> learn_files;
    log_file_ptr log;
    decree last_max_decree = 0;
    int learned_file_head_index = 0;
    int learned_file_tail_index = 0;
    int64_t learned_file_start_offset = 0;
    for (auto itr = files.rbegin(); itr != files.rend(); ++itr) {
        log = itr->second;
        if (log->end_offset() <= _private_log_info.valid_start_offset)
            break;

        if (skip_next) {
            skip_next = (log->previous_log_max_decrees().size() == 0);
            continue;
        }

        if (log->end_offset() > log->start_offset()) {
            // not empty file
            learn_files.push_back(log->path());
            if (learned_file_tail_index == 0)
                learned_file_tail_index = log->index();
            learned_file_head_index = log->index();
            learned_file_start_offset = log->start_offset();
        }

        skip_next = (log->previous_log_max_decrees().size() == 0);

        // continue checking as this file may be a fault
        if (skip_next)
            continue;

        last_max_decree = log->previous_log_max_decrees().begin()->second.max_decree;

        // when all possible decrees are not needed
        if (last_max_decree < start) {
            // skip all older logs
            break;
        }
    }

    // reverse the order, to make files ordered by index incrementally
    state.files.reserve(learn_files.size());
    for (auto it = learn_files.rbegin(); it != learn_files.rend(); ++it) {
        state.files.push_back(*it);
    }

    bool ret = (learned_file_start_offset >= _private_log_info.valid_start_offset &&
                last_max_decree > 0 && last_max_decree < start);
    ddebug("gpid(%d.%d) get_learn_state returns %s, "
           "private logs count %d (%d => %d), learned files count %d (%d => %d): "
           "learned_file_start_offset(%" PRId64 ") >= valid_start_offset(%" PRId64 ") && "
           "last_max_decree(%" PRId64 ") > 0 && last_max_decree(%" PRId64
           ") < learn_start_decree(%" PRId64 ")",
           gpid.get_app_id(),
           gpid.get_partition_index(),
           ret ? "true" : "false",
           (int)files.size(),
           files.empty() ? 0 : files.begin()->first,
           files.empty() ? 0 : files.rbegin()->first,
           (int)learn_files.size(),
           learned_file_head_index,
           learned_file_tail_index,
           learned_file_start_offset,
           _private_log_info.valid_start_offset,
           last_max_decree,
           last_max_decree,
           start);

    return ret;
}

void mutation_log::get_parent_mutations_and_logs(gpid pid,
                                                 decree start_decree,
                                                 ballot start_ballot,
                                                 std::vector<mutation_ptr> &mutation_list,
                                                 std::vector<std::string> &files,
                                                 uint64_t &total_file_size) const
{
    dassert(_is_private, "this method is only valid for private logs");
    dcheck_eq(_private_gpid, pid);

    mutation_list.clear();
    files.clear();
    total_file_size = 0;

    get_in_memory_mutations(start_decree, start_ballot, mutation_list);

    if (mutation_list.size() == 0 && start_decree > _private_log_info.max_decree) {
        // no memory data and no disk data
        return;
    }
    std::map<int, log_file_ptr> file_map = get_log_file_map();

    bool skip_next = false;
    std::list<std::string> learn_files;
    decree last_max_decree = 0;
    for (auto itr = file_map.rbegin(); itr != file_map.rend(); ++itr) {
        log_file_ptr &log = itr->second;
        if (log->end_offset() <= _private_log_info.valid_start_offset)
            break;

        if (skip_next) {
            skip_next = (log->previous_log_max_decrees().size() == 0);
            continue;
        }

        if (log->end_offset() > log->start_offset()) {
            // not empty file
            learn_files.push_back(log->path());
            total_file_size += (log->end_offset() - log->start_offset());
        }

        skip_next = (log->previous_log_max_decrees().size() == 0);
        // continue checking as this file may be a fault
        if (skip_next)
            continue;

        last_max_decree = log->previous_log_max_decrees().begin()->second.max_decree;
        // when all possible decrees are not needed
        if (last_max_decree < start_decree) {
            // skip all older logs
            break;
        }
    }

    // reverse the order, to make files ordered by index incrementally
    files.reserve(learn_files.size());
    for (auto it = learn_files.rbegin(); it != learn_files.rend(); ++it) {
        files.push_back(*it);
    }
}

// return true if the file is covered by both reserve_max_size and reserve_max_time
static bool should_reserve_file(log_file_ptr log,
                                int64_t already_reserved_size,
                                int64_t reserve_max_size,
                                int64_t reserve_max_time)
{
    if (reserve_max_size == 0 || reserve_max_time == 0)
        return false;

    int64_t file_size = log->end_offset() - log->start_offset();
    if (already_reserved_size + file_size > reserve_max_size) {
        // already exceed size limit, should not reserve
        return false;
    }

    uint64_t file_last_write_time = log->last_write_time();
    if (file_last_write_time == 0) {
        time_t tm;
        if (!dsn::utils::filesystem::last_write_time(log->path(), tm)) {
            // get file last write time failed, reserve it for safety
            dwarn("get last write time of file %s failed", log->path().c_str());
            return true;
        }
        file_last_write_time = (uint64_t)tm;
        log->set_last_write_time(file_last_write_time);
    }
    uint64_t current_time = dsn_now_ms() / 1000;
    if (file_last_write_time + reserve_max_time < current_time) {
        // already exceed time limit, should not reserve
        return false;
    }

    // not exceed size and time limit, reserve it
    return true;
}

int mutation_log::garbage_collection(gpid gpid,
                                     decree cleanable_decree,
                                     int64_t valid_start_offset,
                                     int64_t reserve_max_size,
                                     int64_t reserve_max_time)
{
    dassert(_is_private, "this method is only valid for private log");

    std::map<int, log_file_ptr> files;
    decree max_decree = invalid_decree;
    int current_file_index = -1;

    {
        zauto_lock l(_lock);
        files = _log_files;
        max_decree = _private_log_info.max_decree;
        if (_current_log_file != nullptr)
            current_file_index = _current_log_file->index();
    }

    if (files.size() <= 1) {
        // nothing to do
        return 0;
    } else {
        // the last one should be the current log file
        dassert(current_file_index == -1 || files.rbegin()->first == current_file_index,
                "invalid current_file_index, index = %d",
                current_file_index);
    }

    // find the largest file which can be deleted.
    // after iterate, the 'mark_it' will point to the largest file which can be deleted.
    std::map<int, log_file_ptr>::reverse_iterator mark_it;
    int64_t already_reserved_size = 0;
    for (mark_it = files.rbegin(); mark_it != files.rend(); ++mark_it) {
        log_file_ptr log = mark_it->second;
        dassert(mark_it->first == log->index(), "%d VS %d", mark_it->first, log->index());
        // currently, "max_decree" is the max decree covered by this log.

        // reserve current file
        if (current_file_index == log->index()) {
            // not break, go to update max decree
        }

        // reserve if the file is covered by both reserve_max_size and reserve_max_time
        else if (should_reserve_file(
                     log, already_reserved_size, reserve_max_size, reserve_max_time)) {
            // not break, go to update max decree
        }

        // log is invalid, ok to delete
        else if (valid_start_offset >= log->end_offset()) {
            ddebug_f("gc_private @ {}: will remove files {} ~ log.{} because "
                     "valid_start_offset={} outdates log_end_offset={}",
                     _private_gpid,
                     files.begin()->second->path(),
                     log->index(),
                     valid_start_offset,
                     log->end_offset());
            break;
        }

        // all mutations are cleanable, ok to delete
        else if (cleanable_decree >= max_decree) {
            ddebug_f("gc_private @ {}: will remove files {} ~ log.{} because "
                     "cleanable_decree={} outdates max_decree={}",
                     _private_gpid,
                     files.begin()->second->path(),
                     log->index(),
                     cleanable_decree,
                     max_decree);
            break;
        }

        // update max decree for the next log file
        auto &max_decrees = log->previous_log_max_decrees();
        auto it3 = max_decrees.find(gpid);
        dassert(it3 != max_decrees.end(), "impossible for private logs");
        max_decree = it3->second.max_decree;
        already_reserved_size += log->end_offset() - log->start_offset();
    }

    if (mark_it == files.rend()) {
        // no file to delete
        return 0;
    }

    // ok, let's delete files in increasing order of file index
    // to avoid making a hole in the file list
    int largest_to_delete = mark_it->second->index();
    int deleted = 0;
    for (auto it = files.begin(); it != files.end() && it->second->index() <= largest_to_delete;
         ++it) {
        log_file_ptr log = it->second;
        dcheck_eq(it->first, log->index());

        // close first
        log->close();

        // delete file
        auto &fpath = log->path();
        if (!dsn::utils::filesystem::remove_path(fpath)) {
            derror("gc_private @ %d.%d: fail to remove %s, stop current gc cycle ...",
                   _private_gpid.get_app_id(),
                   _private_gpid.get_partition_index(),
                   fpath.c_str());
            break;
        }

        // delete succeed
        ddebug_f("gc_private @ {}: log file {} is removed", _private_gpid, fpath);
        deleted++;

        // erase from _log_files
        {
            zauto_lock l(_lock);
            _log_files.erase(it->first);
            _global_start_offset =
                _log_files.size() > 0 ? _log_files.begin()->second->start_offset() : 0;
        }
    }

    return deleted;
}

int mutation_log::garbage_collection(const replica_log_info_map &gc_condition,
                                     int file_count_limit,
                                     std::set<gpid> &prevent_gc_replicas)
{
    dassert(!_is_private, "this method is only valid for shared log");

    std::map<int, log_file_ptr> files;
    replica_log_info_map max_decrees;
    int current_log_index = -1;
    int64_t total_log_size = 0;

    {
        zauto_lock l(_lock);
        files = _log_files;
        max_decrees = _shared_log_info_map;
        if (_current_log_file != nullptr)
            current_log_index = _current_log_file->index();
        total_log_size = total_size_no_lock();
    }

    if (files.size() <= 1) {
        // nothing to do
        ddebug("gc_shared: too few files to delete, file_count_limit = %d, "
               "reserved_log_count = %d, reserved_log_size = %" PRId64 ", current_log_index = %d",
               file_count_limit,
               (int)files.size(),
               total_log_size,
               current_log_index);
        return (int)files.size();
    } else {
        // the last one should be the current log file
        dassert(-1 == current_log_index || files.rbegin()->first == current_log_index,
                "invalid current_log_index, index = %d",
                current_log_index);
    }

    int reserved_log_count = files.size();
    int64_t reserved_log_size = total_log_size;
    int reserved_smallest_log = files.begin()->first;
    int reserved_largest_log = current_log_index;

    // find the largest file which can be deleted.
    // after iterate, the 'mark_it' will point to the largest file which can be deleted.
    std::map<int, log_file_ptr>::reverse_iterator mark_it;
    std::set<gpid> kickout_replicas;
    gpid stop_gc_replica;
    int stop_gc_log_index = 0;
    decree stop_gc_decree_gap = 0;
    decree stop_gc_garbage_max_decree = 0;
    decree stop_gc_log_max_decree = 0;
    int file_count = 0;
    for (mark_it = files.rbegin(); mark_it != files.rend(); ++mark_it) {
        log_file_ptr log = mark_it->second;
        dassert(mark_it->first == log->index(), "%d VS %d", mark_it->first, log->index());
        file_count++;

        bool delete_ok = true;

        // skip current file
        if (current_log_index == log->index()) {
            delete_ok = false;
        }

        if (delete_ok) {
            std::set<gpid> prevent_gc_replicas_for_this_log;

            for (auto &kv : gc_condition) {
                if (kickout_replicas.find(kv.first) != kickout_replicas.end()) {
                    // no need to consider this replica
                    continue;
                }

                gpid gpid = kv.first;
                decree garbage_max_decree = kv.second.max_decree;
                int64_t valid_start_offset = kv.second.valid_start_offset;

                bool delete_ok_for_this_replica = false;
                bool kickout_this_replica = false;
                auto it3 = max_decrees.find(gpid);

                // log not found for this replica, ok to delete
                if (it3 == max_decrees.end()) {
                    // valid_start_offset may be reset to 0 if initialize_on_load() returns
                    // ERR_INCOMPLETE_DATA
                    dassert(
                        valid_start_offset == 0 || valid_start_offset >= log->end_offset(),
                        "valid start offset must be 0 or greater than the end of this log file");

                    dinfo("gc @ %d.%d: max_decree for %s is missing vs %" PRId64
                          " as garbage max decree,"
                          " safe to delete this and all older logs for this replica",
                          gpid.get_app_id(),
                          gpid.get_partition_index(),
                          log->path().c_str(),
                          garbage_max_decree);
                    delete_ok_for_this_replica = true;
                    kickout_this_replica = true;
                }

                // log is invalid for this replica, ok to delete
                else if (log->end_offset() <= valid_start_offset) {
                    dinfo(
                        "gc @ %d.%d: log is invalid for %s, as"
                        " valid start offset vs log end offset = %" PRId64 " vs %" PRId64 ","
                        " it is therefore safe to delete this and all older logs for this replica",
                        gpid.get_app_id(),
                        gpid.get_partition_index(),
                        log->path().c_str(),
                        valid_start_offset,
                        log->end_offset());
                    delete_ok_for_this_replica = true;
                    kickout_this_replica = true;
                }

                // all decrees are no more than garbage max decree, ok to delete
                else if (it3->second.max_decree <= garbage_max_decree) {
                    dinfo(
                        "gc @ %d.%d: max_decree for %s is %" PRId64 " vs %" PRId64
                        " as garbage max decree,"
                        " it is therefore safe to delete this and all older logs for this replica",
                        gpid.get_app_id(),
                        gpid.get_partition_index(),
                        log->path().c_str(),
                        it3->second.max_decree,
                        garbage_max_decree);
                    delete_ok_for_this_replica = true;
                    kickout_this_replica = true;
                }

                else // it3->second.max_decree > garbage_max_decree
                {
                    // should not delete this file
                    dinfo("gc @ %d.%d: max_decree for %s is %" PRId64 " vs %" PRId64
                          " as garbage max decree,"
                          " it is therefore not allowed to delete this and all older logs",
                          gpid.get_app_id(),
                          gpid.get_partition_index(),
                          log->path().c_str(),
                          it3->second.max_decree,
                          garbage_max_decree);
                    prevent_gc_replicas_for_this_log.insert(gpid);
                    decree gap = it3->second.max_decree - garbage_max_decree;
                    if (log->index() < stop_gc_log_index || gap > stop_gc_decree_gap) {
                        // record the max gap replica for the smallest log
                        stop_gc_replica = gpid;
                        stop_gc_log_index = log->index();
                        stop_gc_decree_gap = gap;
                        stop_gc_garbage_max_decree = garbage_max_decree;
                        stop_gc_log_max_decree = it3->second.max_decree;
                    }
                }

                if (kickout_this_replica) {
                    // files before this file is useless for this replica,
                    // so from now on, this replica will not be considered anymore
                    kickout_replicas.insert(gpid);
                }

                if (!delete_ok_for_this_replica) {
                    // can not delete this file, mark it, and continue to check other replicas
                    delete_ok = false;
                }
            }

            // update prevent_gc_replicas
            if (file_count > file_count_limit && !prevent_gc_replicas_for_this_log.empty()) {
                prevent_gc_replicas.insert(prevent_gc_replicas_for_this_log.begin(),
                                           prevent_gc_replicas_for_this_log.end());
            }
        }

        if (delete_ok) {
            // found the largest file which can be deleted
            break;
        }

        // update max_decrees for the next log file
        max_decrees = log->previous_log_max_decrees();
    }

    if (mark_it == files.rend()) {
        // no file to delete
        if (stop_gc_decree_gap > 0) {
            ddebug("gc_shared: no file can be deleted, file_count_limit = %d, "
                   "reserved_log_count = %d, reserved_log_size = %" PRId64 ", "
                   "reserved_smallest_log = %d, reserved_largest_log = %d, "
                   "stop_gc_log_index = %d, stop_gc_replica_count = %d, "
                   "stop_gc_replica = %d.%d, stop_gc_decree_gap = %" PRId64 ", "
                   "stop_gc_garbage_max_decree = %" PRId64 ", stop_gc_log_max_decree = %" PRId64 "",
                   file_count_limit,
                   reserved_log_count,
                   reserved_log_size,
                   reserved_smallest_log,
                   reserved_largest_log,
                   stop_gc_log_index,
                   (int)prevent_gc_replicas.size(),
                   stop_gc_replica.get_app_id(),
                   stop_gc_replica.get_partition_index(),
                   stop_gc_decree_gap,
                   stop_gc_garbage_max_decree,
                   stop_gc_log_max_decree);
        } else {
            ddebug("gc_shared: no file can be deleted, file_count_limit = %d, "
                   "reserved_log_count = %d, reserved_log_size = %" PRId64 ", "
                   "reserved_smallest_log = %d, reserved_largest_log = %d, ",
                   file_count_limit,
                   reserved_log_count,
                   reserved_log_size,
                   reserved_smallest_log,
                   reserved_largest_log);
        }

        return reserved_log_count;
    }

    // ok, let's delete files in increasing order of file index
    // to avoid making a hole in the file list
    int largest_log_to_delete = mark_it->second->index();
    int to_delete_log_count = 0;
    int64_t to_delete_log_size = 0;
    int deleted_log_count = 0;
    int64_t deleted_log_size = 0;
    int deleted_smallest_log = 0;
    int deleted_largest_log = 0;
    for (auto it = files.begin(); it != files.end() && it->second->index() <= largest_log_to_delete;
         ++it) {
        log_file_ptr log = it->second;
        dassert(it->first == log->index(), "%d VS %d", it->first, log->index());
        to_delete_log_count++;
        to_delete_log_size += log->end_offset() - log->start_offset();

        // close first
        log->close();

        // delete file
        auto &fpath = log->path();
        if (!dsn::utils::filesystem::remove_path(fpath)) {
            derror("gc_shared: fail to remove %s, stop current gc cycle ...", fpath.c_str());
            break;
        }

        // delete succeed
        ddebug("gc_shared: log file %s is removed", fpath.c_str());
        deleted_log_count++;
        deleted_log_size += log->end_offset() - log->start_offset();
        if (deleted_smallest_log == 0)
            deleted_smallest_log = log->index();
        deleted_largest_log = log->index();

        // erase from _log_files
        {
            zauto_lock l(_lock);
            _log_files.erase(it->first);
            _global_start_offset =
                _log_files.size() > 0 ? _log_files.begin()->second->start_offset() : 0;
            reserved_log_count = _log_files.size();
            reserved_log_size = total_size_no_lock();
            if (reserved_log_count > 0) {
                reserved_smallest_log = _log_files.begin()->first;
                reserved_largest_log = _log_files.rbegin()->first;
            } else {
                reserved_smallest_log = -1;
                reserved_largest_log = -1;
            }
        }
    }

    if (stop_gc_decree_gap > 0) {
        ddebug("gc_shared: deleted some files, file_count_limit = %d, "
               "reserved_log_count = %d, reserved_log_size = %" PRId64 ", "
               "reserved_smallest_log = %d, reserved_largest_log = %d, "
               "to_delete_log_count = %d, to_delete_log_size = %" PRId64 ", "
               "deleted_log_count = %d, deleted_log_size = %" PRId64 ", "
               "deleted_smallest_log = %d, deleted_largest_log = %d, "
               "stop_gc_log_index = %d, stop_gc_replica_count = %d, "
               "stop_gc_replica = %d.%d, stop_gc_decree_gap = %" PRId64 ", "
               "stop_gc_garbage_max_decree = %" PRId64 ", stop_gc_log_max_decree = %" PRId64 "",
               file_count_limit,
               reserved_log_count,
               reserved_log_size,
               reserved_smallest_log,
               reserved_largest_log,
               to_delete_log_count,
               to_delete_log_size,
               deleted_log_count,
               deleted_log_size,
               deleted_smallest_log,
               deleted_largest_log,
               stop_gc_log_index,
               (int)prevent_gc_replicas.size(),
               stop_gc_replica.get_app_id(),
               stop_gc_replica.get_partition_index(),
               stop_gc_decree_gap,
               stop_gc_garbage_max_decree,
               stop_gc_log_max_decree);
    } else {
        ddebug("gc_shared: deleted some files, file_count_limit = %d, "
               "reserved_log_count = %d, reserved_log_size = %" PRId64 ", "
               "reserved_smallest_log = %d, reserved_largest_log = %d, "
               "to_delete_log_count = %d, to_delete_log_size = %" PRId64 ", "
               "deleted_log_count = %d, deleted_log_size = %" PRId64 ", "
               "deleted_smallest_log = %d, deleted_largest_log = %d",
               file_count_limit,
               reserved_log_count,
               reserved_log_size,
               reserved_smallest_log,
               reserved_largest_log,
               to_delete_log_count,
               to_delete_log_size,
               deleted_log_count,
               deleted_log_size,
               deleted_smallest_log,
               deleted_largest_log);
    }

    return reserved_log_count;
}

std::map<int, log_file_ptr> mutation_log::get_log_file_map() const
{
    zauto_lock l(_lock);
    return _log_files;
}

// log_file::file_streamer
class log_file::file_streamer
{
public:
    explicit file_streamer(disk_file *fd, size_t file_offset)
        : _file_dispatched_bytes(file_offset), _file_handle(fd)
    {
        _current_buffer = _buffers + 0;
        _next_buffer = _buffers + 1;
        fill_buffers();
    }
    ~file_streamer()
    {
        _current_buffer->wait_ongoing_task();
        _next_buffer->wait_ongoing_task();
    }
    // try to reset file_offset
    void reset(size_t file_offset)
    {
        _current_buffer->wait_ongoing_task();
        _next_buffer->wait_ongoing_task();
        // fast path if we can just move the cursor
        if (_current_buffer->_file_offset_of_buffer <= file_offset &&
            _current_buffer->_file_offset_of_buffer + _current_buffer->_end > file_offset) {
            _current_buffer->_begin = file_offset - _current_buffer->_file_offset_of_buffer;
        } else {
            _current_buffer->_begin = _current_buffer->_end = _next_buffer->_begin =
                _next_buffer->_end = 0;
            _file_dispatched_bytes = file_offset;
        }
        fill_buffers();
    }

    // TODO(wutao1): use string_view instead of using blob.
    // WARNING: the resulted blob is not guaranteed to be reference counted.
    // possible error_code:
    //  ERR_OK                      result would always size as expected
    //  ERR_HANDLE_EOF              if there are not enough data in file. result would still be
    //                              filled with possible data
    //  ERR_FILE_OPERATION_FAILED   filesystem failure
    error_code read_next(size_t size, /*out*/ blob &result)
    {
        binary_writer writer(size);

#define TRY(x)                                                                                     \
    do {                                                                                           \
        auto _x = (x);                                                                             \
        if (_x != ERR_OK) {                                                                        \
            result = writer.get_current_buffer();                                                  \
            return _x;                                                                             \
        }                                                                                          \
    } while (0)

        TRY(_current_buffer->wait_ongoing_task());
        if (size < _current_buffer->length()) {
            result.assign(_current_buffer->_buffer.get(), _current_buffer->_begin, size);
            _current_buffer->_begin += size;
        } else {
            _current_buffer->drain(writer);
            // we can now assign result since writer must have allocated a buffer.
            dassert(writer.total_size() != 0, "writer.total_size = %d", writer.total_size());
            if (size > writer.total_size()) {
                TRY(_next_buffer->wait_ongoing_task());
                _next_buffer->consume(writer,
                                      std::min(size - writer.total_size(), _next_buffer->length()));
                // We hope that this never happens, it would deteriorate performance
                if (size > writer.total_size()) {
                    auto task =
                        file::read(_file_handle,
                                   writer.get_current_buffer().buffer().get() + writer.total_size(),
                                   size - writer.total_size(),
                                   _file_dispatched_bytes,
                                   LPC_AIO_IMMEDIATE_CALLBACK,
                                   nullptr,
                                   nullptr);
                    task->wait();
                    writer.write_empty(task->get_transferred_size());
                    _file_dispatched_bytes += task->get_transferred_size();
                    TRY(task->error());
                }
            }
            result = writer.get_current_buffer();
        }
        fill_buffers();
        return ERR_OK;
#undef TRY
    }

private:
    void fill_buffers()
    {
        while (!_current_buffer->_have_ongoing_task && _current_buffer->empty()) {
            _current_buffer->_begin = _current_buffer->_end = 0;
            _current_buffer->_file_offset_of_buffer = _file_dispatched_bytes;
            _current_buffer->_have_ongoing_task = true;
            _current_buffer->_task = file::read(_file_handle,
                                                _current_buffer->_buffer.get(),
                                                block_size_bytes,
                                                _file_dispatched_bytes,
                                                LPC_AIO_IMMEDIATE_CALLBACK,
                                                nullptr,
                                                nullptr);
            _file_dispatched_bytes += block_size_bytes;
            std::swap(_current_buffer, _next_buffer);
        }
    }

    // buffer size, in bytes
    // TODO(wutao1): call it BLOCK_BYTES_SIZE
    static constexpr size_t block_size_bytes = 1024 * 1024; // 1MB
    struct buffer_t
    {
        std::unique_ptr<char[]> _buffer; // with block_size
        size_t _begin, _end;             // [buffer[begin]..buffer[end]) contains unconsumed_data
        size_t _file_offset_of_buffer;   // file offset projected to buffer[0]
        bool _have_ongoing_task;
        aio_task_ptr _task;

        buffer_t()
            : _buffer(new char[block_size_bytes]),
              _begin(0),
              _end(0),
              _file_offset_of_buffer(0),
              _have_ongoing_task(false)
        {
        }
        size_t length() const { return _end - _begin; }
        bool empty() const { return length() == 0; }
        void consume(binary_writer &dest, size_t len)
        {
            dest.write(_buffer.get() + _begin, len);
            _begin += len;
        }
        size_t drain(binary_writer &dest)
        {
            auto len = length();
            consume(dest, len);
            return len;
        }
        error_code wait_ongoing_task()
        {
            if (_have_ongoing_task) {
                _task->wait();
                _have_ongoing_task = false;
                _end += _task->get_transferred_size();
                dassert(_end <= block_size_bytes, "invalid io_size.");
                return _task->error();
            } else {
                return ERR_OK;
            }
        }
    } _buffers[2];
    buffer_t *_current_buffer, *_next_buffer;

    // number of bytes we have issued read operations
    size_t _file_dispatched_bytes;
    disk_file *_file_handle;
};

//------------------- log_file --------------------------
log_file::~log_file() { close(); }
/*static */ log_file_ptr log_file::open_read(const char *path, /*out*/ error_code &err)
{
    char splitters[] = {'\\', '/', 0};
    std::string name = utils::get_last_component(std::string(path), splitters);

    // log.index.start_offset
    if (name.length() < strlen("log.") || name.substr(0, strlen("log.")) != std::string("log.")) {
        err = ERR_INVALID_PARAMETERS;
        dwarn("invalid log path %s", path);
        return nullptr;
    }

    auto pos = name.find_first_of('.');
    dassert(pos != std::string::npos, "invalid log_file, name = %s", name.c_str());
    auto pos2 = name.find_first_of('.', pos + 1);
    if (pos2 == std::string::npos) {
        err = ERR_INVALID_PARAMETERS;
        dwarn("invalid log path %s", path);
        return nullptr;
    }

    /* so the log file format is log.index_str.start_offset_str */
    std::string index_str = name.substr(pos + 1, pos2 - pos - 1);
    std::string start_offset_str = name.substr(pos2 + 1);
    if (index_str.empty() || start_offset_str.empty()) {
        err = ERR_INVALID_PARAMETERS;
        dwarn("invalid log path %s", path);
        return nullptr;
    }

    char *p = nullptr;
    int index = static_cast<int>(strtol(index_str.c_str(), &p, 10));
    if (*p != 0) {
        err = ERR_INVALID_PARAMETERS;
        dwarn("invalid log path %s", path);
        return nullptr;
    }
    int64_t start_offset = static_cast<int64_t>(strtoll(start_offset_str.c_str(), &p, 10));
    if (*p != 0) {
        err = ERR_INVALID_PARAMETERS;
        dwarn("invalid log path %s", path);
        return nullptr;
    }

    disk_file *hfile = file::open(path, O_RDONLY | O_BINARY, 0);
    if (!hfile) {
        err = ERR_FILE_OPERATION_FAILED;
        dwarn("open log file %s failed", path);
        return nullptr;
    }

    auto lf = new log_file(path, hfile, index, start_offset, true);
    lf->reset_stream();
    blob hdr_blob;
    err = lf->read_next_log_block(hdr_blob);
    if (err == ERR_INVALID_DATA || err == ERR_INCOMPLETE_DATA || err == ERR_HANDLE_EOF ||
        err == ERR_FILE_OPERATION_FAILED) {
        std::string removed = std::string(path) + ".removed";
        derror("read first log entry of file %s failed, err = %s. Rename the file to %s",
               path,
               err.to_string(),
               removed.c_str());
        delete lf;
        lf = nullptr;

        // rename file on failure
        dsn::utils::filesystem::rename_path(path, removed);

        return nullptr;
    }

    binary_reader reader(std::move(hdr_blob));
    lf->read_file_header(reader);
    if (!lf->is_right_header()) {
        std::string removed = std::string(path) + ".removed";
        derror("invalid log file header of file %s. Rename the file to %s", path, removed.c_str());
        delete lf;
        lf = nullptr;

        // rename file on failure
        dsn::utils::filesystem::rename_path(path, removed);

        err = ERR_INVALID_DATA;
        return nullptr;
    }

    err = ERR_OK;
    return lf;
}

/*static*/ log_file_ptr log_file::create_write(const char *dir, int index, int64_t start_offset)
{
    char path[512];
    sprintf(path, "%s/log.%d.%" PRId64, dir, index, start_offset);

    if (dsn::utils::filesystem::path_exists(std::string(path))) {
        dwarn("log file %s already exist", path);
        return nullptr;
    }

    disk_file *hfile = file::open(path, O_RDWR | O_CREAT | O_BINARY, 0666);
    if (!hfile) {
        dwarn("create log %s failed", path);
        return nullptr;
    }

    return new log_file(path, hfile, index, start_offset, false);
}

log_file::log_file(
    const char *path, disk_file *handle, int index, int64_t start_offset, bool is_read)
    : _is_read(is_read)
{
    _start_offset = start_offset;
    _end_offset = start_offset;
    _handle = handle;
    _path = path;
    _index = index;
    _crc32 = 0;
    _last_write_time = 0;
    memset(&_header, 0, sizeof(_header));

    if (is_read) {
        int64_t sz;
        if (!dsn::utils::filesystem::file_size(_path, sz)) {
            dassert(false, "fail to get file size of %s.", _path.c_str());
        }
        _end_offset += sz;
    }
}

void log_file::close()
{
    zauto_lock lock(_write_lock);

    //_stream implicitly refer to _handle so it needs to be cleaned up first.
    // TODO: We need better abstraction to avoid those manual stuffs..
    _stream.reset(nullptr);
    if (_handle) {
        error_code err = file::close(_handle);
        dassert(err == ERR_OK, "file::close failed, err = %s", err.to_string());

        _handle = nullptr;
    }
}

void log_file::flush() const
{
    dassert(!_is_read, "log file must be of write mode");
    zauto_lock lock(_write_lock);

    if (_handle) {
        error_code err = file::flush(_handle);
        dassert(err == ERR_OK, "file::flush failed, err = %s", err.to_string());
    }
}

error_code log_file::read_next_log_block(/*out*/ ::dsn::blob &bb)
{
    dassert(_is_read, "log file must be of read mode");
    auto err = _stream->read_next(sizeof(log_block_header), bb);
    if (err != ERR_OK || bb.length() != sizeof(log_block_header)) {
        if (err == ERR_OK || err == ERR_HANDLE_EOF) {
            // if read_count is 0, then we meet the end of file
            err = (bb.length() == 0 ? ERR_HANDLE_EOF : ERR_INCOMPLETE_DATA);
        } else {
            derror("read data block header failed, size = %d vs %d, err = %s",
                   bb.length(),
                   (int)sizeof(log_block_header),
                   err.to_string());
        }

        return err;
    }
    log_block_header hdr = *reinterpret_cast<const log_block_header *>(bb.data());

    if (hdr.magic != 0xdeadbeef) {
        derror("invalid data header magic: 0x%x", hdr.magic);
        return ERR_INVALID_DATA;
    }

    err = _stream->read_next(hdr.length, bb);
    if (err != ERR_OK || hdr.length != bb.length()) {
        derror("read data block body failed, size = %d vs %d, err = %s",
               bb.length(),
               (int)hdr.length,
               err.to_string());

        if (err == ERR_OK || err == ERR_HANDLE_EOF) {
            // because already read log_block_header above, so here must be imcomplete data
            err = ERR_INCOMPLETE_DATA;
        }

        return err;
    }

    auto crc = dsn::utils::crc32_calc(
        static_cast<const void *>(bb.data()), static_cast<size_t>(hdr.length), _crc32);
    if (crc != hdr.body_crc) {
        derror("crc checking failed");
        return ERR_INVALID_DATA;
    }
    _crc32 = crc;

    return ERR_OK;
}

aio_task_ptr log_file::commit_log_block(log_block &block,
                                        int64_t offset,
                                        dsn::task_code evt,
                                        dsn::task_tracker *tracker,
                                        aio_handler &&callback,
                                        int hash)
{
    log_appender pending(offset, block);
    return commit_log_blocks(pending, evt, tracker, std::move(callback), hash);
}
aio_task_ptr log_file::commit_log_blocks(log_appender &pending,
                                         dsn::task_code evt,
                                         dsn::task_tracker *tracker,
                                         aio_handler &&callback,
                                         int hash)
{
    dassert(!_is_read, "log file must be of write mode");
    dcheck_gt(pending.size(), 0);

    zauto_lock lock(_write_lock);
    if (!_handle) {
        return nullptr;
    }

    auto size = (long long)pending.size();
    size_t vec_size = pending.blob_count();
    std::vector<dsn_file_buffer_t> buffer_vector(vec_size);
    int buffer_idx = 0;
    for (log_block &block : pending.all_blocks()) {
        int64_t local_offset = block.start_offset() - start_offset();
        auto hdr = reinterpret_cast<log_block_header *>(const_cast<char *>(block.front().data()));

        dassert(hdr->magic == 0xdeadbeef, "");
        hdr->local_offset = local_offset;
        hdr->length = static_cast<int32_t>(block.size() - sizeof(log_block_header));
        hdr->body_crc = _crc32;

        for (int i = 0; i < block.data().size(); i++) {
            auto &blk = block.data()[i];
            buffer_vector[buffer_idx].buffer =
                reinterpret_cast<void *>(const_cast<char *>(blk.data()));
            buffer_vector[buffer_idx].size = blk.length();

            // skip block header
            if (i > 0) {
                hdr->body_crc = dsn::utils::crc32_calc(static_cast<const void *>(blk.data()),
                                                       static_cast<size_t>(blk.length()),
                                                       hdr->body_crc);
            }
            buffer_idx++;
        }
        _crc32 = hdr->body_crc;
    }

    aio_task_ptr tsk;
    int64_t local_offset = pending.start_offset() - start_offset();
    if (callback) {
        tsk = file::write_vector(_handle,
                                 buffer_vector.data(),
                                 vec_size,
                                 static_cast<uint64_t>(local_offset),
                                 evt,
                                 tracker,
                                 std::forward<aio_handler>(callback),
                                 hash);
    } else {
        tsk = file::write_vector(_handle,
                                 buffer_vector.data(),
                                 vec_size,
                                 static_cast<uint64_t>(local_offset),
                                 evt,
                                 tracker,
                                 nullptr,
                                 hash);
    }

    _end_offset.fetch_add(size);
    return tsk;
}

void log_file::reset_stream(size_t offset /*default = 0*/)
{
    if (_stream == nullptr) {
        _stream.reset(new file_streamer(_handle, offset));
    } else {
        _stream->reset(offset);
    }
    if (offset == 0) {
        _crc32 = 0;
    }
}

decree log_file::previous_log_max_decree(const dsn::gpid &pid)
{
    auto it = _previous_log_max_decrees.find(pid);
    return it == _previous_log_max_decrees.end() ? 0 : it->second.max_decree;
}

int log_file::read_file_header(binary_reader &reader)
{
    /*
     * the log file header structure:
     *   log_file_header +
     *   count + count * (gpid + replica_log_info)
     */
    reader.read_pod(_header);

    int count;
    reader.read(count);
    for (int i = 0; i < count; i++) {
        gpid gpid;
        replica_log_info info;

        reader.read_pod(gpid);
        reader.read_pod(info);

        _previous_log_max_decrees[gpid] = info;
    }

    return get_file_header_size();
}

int log_file::get_file_header_size() const
{
    int count = static_cast<int>(_previous_log_max_decrees.size());
    return static_cast<int>(sizeof(log_file_header) + sizeof(count) +
                            (sizeof(gpid) + sizeof(replica_log_info)) * count);
}

bool log_file::is_right_header() const
{
    return _header.magic == 0xdeadbeef && _header.start_global_offset == _start_offset;
}

int log_file::write_file_header(binary_writer &writer, const replica_log_info_map &init_max_decrees)
{
    /*
     * the log file header structure:
     *   log_file_header +
     *   count + count * (gpid + replica_log_info)
     */
    _previous_log_max_decrees = init_max_decrees;

    _header.magic = 0xdeadbeef;
    _header.version = 0x1;
    _header.start_global_offset = start_offset();

    writer.write_pod(_header);

    int count = static_cast<int>(_previous_log_max_decrees.size());
    writer.write(count);
    for (auto &kv : _previous_log_max_decrees) {
        writer.write_pod(kv.first);
        writer.write_pod(kv.second);
    }

    return get_file_header_size();
}
} // namespace replication
} // namespace dsn
