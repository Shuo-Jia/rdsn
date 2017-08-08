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

/*
 * Description:
 *     fs_manager's header: used to track the disk position for all the allocated replicas
 *
 * Revision history:
 *     2017-08-08: sunweijie@xiaomi.com, first draft
 */

#pragma once

#include <dsn/service_api_cpp.h>
#include <dsn/cpp/zlocks.h>
#include "replication_common.h"

namespace dsn {
namespace replication {

struct dir_node
{
public:
    dir_node() : disk_capacity_mb(0) {}
    std::string tag;
    std::string full_dir;
    int64_t disk_capacity_mb;
    std::map<app_id, std::set<gpid>> holding_replicas;

public:
    unsigned replicas_count(app_id id) const;
    unsigned replicas_count() const;
    bool has(const dsn::gpid &pid) const;
    unsigned remove(const dsn::gpid &pid);
};

class fs_manager
{
public:
    fs_manager() {}
    ~fs_manager() {}

    // this should be called before open/load any replicas
    dsn::error_code initialize(const replication_options &opts);
    dsn::error_code initialize(const std::vector<std::string> &data_dirs,
                               const std::vector<std::string> &tags);

    dsn::error_code get_disk_tag(const std::string &dir, /*out*/ std::string &tag);
    void allocate_dir(const dsn::gpid &pid,
                      const std::string &type,
                      /*out*/ std::string &dir);
    void add_replica(const dsn::gpid &pid, const std::string &pid_dir);
    void remove_replica(const dsn::gpid &pid);
    bool for_each_dir_node(const std::function<bool(const dir_node &)> &func) const;

private:
    dir_node *get_dir_node(const std::string &subdir);

    // when visit the tag/storage of the _dir_nodes map, there's no need to protect by the lock.
    // but when visit the holding_replicas, you must take care.
    mutable zrwlock_nr _lock;
    // data_dir_tag -> dir_node
    std::map<std::string, dir_node> _dir_nodes;
};
}
}