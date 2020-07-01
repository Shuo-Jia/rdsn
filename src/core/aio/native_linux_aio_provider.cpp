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

#include "native_linux_aio_provider.h"

#include <fcntl.h>
#include <cstdlib>
#include <dsn/dist/fmt_logging.h>
#include <dsn/perf_counter/perf_counter_wrapper.h>

namespace dsn {

dsn::perf_counter_wrapper _total_native_aio_count;
dsn::perf_counter_wrapper _total_native_aio_submit_count;
dsn::perf_counter_wrapper _native_aio_submit_latency;
;

native_linux_aio_provider::native_linux_aio_provider(disk_engine *disk,
                                                     aio_provider *inner_provider)
    : aio_provider(disk, inner_provider)
{
    for (int i = 0; i < 2; i++) {
        memset(&_ctx[i], 0, sizeof(_ctx[i]));
        auto ret = io_setup(128, &_ctx[i]);
        dassert(ret == 0, "io_setup error, ret = %d", ret);
        _is_running = true;
        _worker[i] = std::thread([this, i, disk]() {
            task::set_tls_dsn_context(node(), nullptr);
            get_event(i);
        });
    } // 128 concurrent events

    static std::once_flag flag;
    std::call_once(flag, [&]() {
        _total_native_aio_count.init_global_counter(
            "replica",
            "app.pegasus",
            "total_native_aio_count",
            COUNTER_TYPE_NUMBER,
            "statistic the memory usage of rocksdb block cache");

        _total_native_aio_submit_count.init_global_counter(
            "replica",
            "app.pegasus",
            "total_native_aio_count",
            COUNTER_TYPE_NUMBER,
            "statistic the memory usage of rocksdb block cache");

        _native_aio_submit_latency.init_global_counter(
            "replica",
            "app.pegasus",
            "native_aio_submit_latency",
            COUNTER_TYPE_NUMBER_PERCENTILES,
            "statistic the through bytes of rocksdb write rate limiter");

    });
}

native_linux_aio_provider::~native_linux_aio_provider()
{
    if (!_is_running) {
        return;
    }
    _is_running = false;

    auto ret = io_destroy(_ctx[0]);
    dassert(ret == 0, "io_destroy error, ret = %d", ret);
    auto ret1 = io_destroy(_ctx[1]);
    dassert(ret1 == 0, "io_destroy error, ret = %d", ret);

    _worker[0].join();
    _worker[1].join();
}

dsn_handle_t native_linux_aio_provider::open(const char *file_name, int flag, int pmode)
{
    flag = flag | O_DIRECT;
    dsn_handle_t fh = (dsn_handle_t)(uintptr_t)::open(file_name, flag, pmode);
    if (fh == DSN_INVALID_FILE_HANDLE) {
        derror("create file failed, err = %s", strerror(errno));
    }
    return fh;
}

error_code native_linux_aio_provider::close(dsn_handle_t fh)
{
    if (fh == DSN_INVALID_FILE_HANDLE || ::close((int)(uintptr_t)(fh)) == 0) {
        return ERR_OK;
    } else {
        derror("close file failed, err = %s", strerror(errno));
        return ERR_FILE_OPERATION_FAILED;
    }
}

error_code native_linux_aio_provider::flush(dsn_handle_t fh)
{
    if (fh == DSN_INVALID_FILE_HANDLE || ::fsync((int)(uintptr_t)(fh)) == 0) {
        return ERR_OK;
    } else {
        derror("flush file failed, err = %s", strerror(errno));
        return ERR_FILE_OPERATION_FAILED;
    }
}

aio_context *native_linux_aio_provider::prepare_aio_context(aio_task *tsk)
{
    return new linux_disk_aio_context(tsk);
}

void native_linux_aio_provider::aio(aio_task *aio_tsk) { aio_internal(aio_tsk, true); }

void native_linux_aio_provider::get_event(int id)
{
    struct io_event events[1];
    int ret;

    task::set_tls_dsn_context(node(), nullptr);

    const char *name = ::dsn::tools::get_service_node_name(node());
    char buffer[128];
    sprintf(buffer, "%s.aio", name);
    task_worker::set_name(buffer);

    while (true) {
        if (dsn_unlikely(!_is_running.load(std::memory_order_relaxed))) {
            break;
        }
        ret = io_getevents(_ctx[id], 1, 1, events, NULL);
        if (ret > 0) // should be 1
        {
            dassert(ret == 1, "io_getevents returns %d", ret);
            struct iocb *io = events[0].obj;
            complete_aio(io, static_cast<int>(events[0].res), static_cast<int>(events[0].res2));
            _total_native_aio_count->decrement();
        } else {
            // on error it returns a negated error number (the negative of one of the values listed
            // in ERRORS
            dwarn("io_getevents returns %d, you probably want to try on another machine:-(", ret);
        }
    }
}

void native_linux_aio_provider::complete_aio(struct iocb *io, int bytes, int err)
{
    linux_disk_aio_context *aio = CONTAINING_RECORD(io, linux_disk_aio_context, cb);
    error_code ec;
    if (err != 0) {
        derror("aio error, err = %s", strerror(err));
        ec = ERR_FILE_OPERATION_FAILED;
    } else {
        ec = bytes > 0 ? ERR_OK : ERR_HANDLE_EOF;
    }

    if (!aio->evt) {
        aio_task *aio_ptr(aio->tsk);
        aio->this_->complete_io(aio_ptr, ec, bytes);
    } else {
        aio->err = ec;
        aio->bytes = bytes;
        aio->evt->notify();
    }
}

error_code native_linux_aio_provider::aio_internal(aio_task *aio_tsk,
                                                   bool async,
                                                   /*out*/ uint32_t *pbytes /*= nullptr*/)
{
    struct iocb *cbs[1];
    linux_disk_aio_context *aio;
    int ret;
    int aio_context_id = aio_tsk->_io_context_id;

    aio = (linux_disk_aio_context *)aio_tsk->get_aio_context();

    memset(&aio->cb, 0, sizeof(aio->cb));

    aio->this_ = this;

    switch (aio->type) {
    case AIO_Read:
        posix_memalign(const_cast<void **>(&aio->buffer), 512, 1024 * 1024);
        io_prep_pread(&aio->cb,
                      static_cast<int>((ssize_t)aio->file),
                      aio->buffer,
                      aio->buffer_size,
                      aio->file_offset);
        break;
    case AIO_Write:
        if (aio->buffer) {
            posix_memalign(const_cast<void **>(&aio->buffer), 512, 1024 * 1024);
            io_prep_pwrite(&aio->cb,
                           static_cast<int>((ssize_t)aio->file),
                           aio->buffer,
                           aio->buffer_size,
                           aio->file_offset);
        } else {
            int iovcnt = aio->write_buffer_vec->size();
            struct iovec *iov = (struct iovec *)alloca(sizeof(struct iovec) * iovcnt);
            for (int i = 0; i < iovcnt; i++) {
                const dsn_file_buffer_t &buf = aio->write_buffer_vec->at(i);
                posix_memalign(const_cast<void **>(&(buf.buffer)), 512, 1024 * 1024);
                iov[i].iov_base = buf.buffer;
                iov[i].iov_len = buf.size;
            }
            io_prep_pwritev(
                &aio->cb, static_cast<int>((ssize_t)aio->file), iov, iovcnt, aio->file_offset);
        }
        break;
    default:
        derror("unknown aio type %u", static_cast<int>(aio->type));
    }

    if (!async) {
        aio->evt = new utils::notify_event();
        aio->err = ERR_OK;
        aio->bytes = 0;
    }

    cbs[0] = &aio->cb;
    uint64_t start_time = dsn_now_ns();
    ret = io_submit(_ctx[aio_context_id], 1, cbs);
    uint64_t time_used = dsn_now_ns() - start_time;
    _native_aio_submit_latency->set(time_used);
    if (time_used > 20000000) {
        derror_f("aio_submit:{}, context_id:{}, type:{}", time_used, aio_context_id, aio->type);
    }
    _total_native_aio_submit_count->increment();
    _total_native_aio_count->increment();

    if (ret != 1) {
        if (ret < 0)
            derror("io_submit error, ret = %d", ret);
        else
            derror("could not sumbit IOs, ret = %d", ret);

        if (async) {
            complete_io(aio_tsk, ERR_FILE_OPERATION_FAILED, 0);
        } else {
            delete aio->evt;
            aio->evt = nullptr;
        }
        return ERR_FILE_OPERATION_FAILED;
    } else {
        if (async) {
            return ERR_IO_PENDING;
        } else {
            aio->evt->wait();
            delete aio->evt;
            aio->evt = nullptr;
            if (pbytes != nullptr) {
                *pbytes = aio->bytes;
            }
            return aio->err;
        }
    }
}

} // namespace dsn
