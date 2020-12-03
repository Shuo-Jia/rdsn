/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <gtest/gtest.h>
#include <dsn/utility/fail_point.h>
#include <dsn/dist/fmt_logging.h>

#include "replica/test/replica_test_base.h"

namespace dsn {
namespace replication {

class replica_disk_test_base : public replica_test_base
{
public:
    // create `dir_nodes_count`(tag_1~tag_5) mock disk:
    // capacity info
    // node_disk     disk_capacity  disk_available_mb  disk_available_ratio
    //  tag_1            100*5             50*1              10%
    //  tag_2            100*5             50*2              20%
    //  tag_3            100*5             50*3              30%
    //  tag_4            100*5             50*4              40%
    //  tag_5            100*5             50*5              50%
    //  total            2500              750               30%
    // replica info, for example:
    //   dir_node             primary/secondary
    //
    //   tag_empty_1
    //   tag_1                1.1 | 1.2,1.3
    //                        2.1,2.2 | 2.3,2.4,2.5,2.6
    //
    //   tag_2                1.4 | 1.5,1.6
    //                        2.7,2.8 | 2.9,2.10,2.11,2.12,2.13
    //            ...
    //            ...
    replica_disk_test_base()
    {
        fail::setup();

        fail::cfg("mock_dir_node", "return()");
        generate_mock_app_info();

        generate_mock_dir_nodes(dir_nodes_count);
        generate_mock_empty_dir_node(empty_dir_nodes_count);

        stub->generate_replicas_base_dir_nodes_for_app(
            app_info_1, app_id_1_primary_count_for_disk, app_id_1_secondary_count_for_disk);

        stub->generate_replicas_base_dir_nodes_for_app(
            app_info_2, app_id_2_primary_count_for_disk, app_id_2_secondary_count_for_disk);
        stub->on_disk_stat();
    }

    ~replica_disk_test_base() { fail::teardown(); }

    void update_disk_replica() { stub->on_disk_stat(); }

    std::vector<std::shared_ptr<dir_node>> get_dir_nodes() { return stub->_fs_manager._dir_nodes; }

    void generate_mock_dir_node(const app_info &app,
                                const gpid pid,
                                const std::string &tag,
                                const std::string &full_dir = "full_dir")
    {
        dir_node *node_disk = new dir_node(tag, full_dir);
        node_disk->holding_replicas[app.app_id].emplace(pid);
        stub->_fs_manager._dir_nodes.emplace_back(node_disk);
    }

    void remove_mock_dir_node(const std::string &tag)
    {
        for (auto iter = stub->_fs_manager._dir_nodes.begin();
             iter != stub->_fs_manager._dir_nodes.end();
             iter++) {
            if ((*iter)->tag == tag) {
                stub->_fs_manager._dir_nodes.erase(iter);
                break;
            }
        }
    }

public:
    int empty_dir_nodes_count = 1;
    int dir_nodes_count = 5;

    dsn::app_info app_info_1;
    int app_id_1_primary_count_for_disk = 1;
    int app_id_1_secondary_count_for_disk = 2;

    dsn::app_info app_info_2;
    int app_id_2_primary_count_for_disk = 2;
    int app_id_2_secondary_count_for_disk = 4;

private:
    void generate_mock_app_info()
    {
        app_info_1.app_id = 1;
        app_info_1.app_name = "disk_test_1";
        app_info_1.app_type = "replica";
        app_info_1.is_stateful = true;
        app_info_1.max_replica_count = 3;
        app_info_1.partition_count = 8;

        app_info_2.app_id = 2;
        app_info_2.app_name = "disk_test_2";
        app_info_2.app_type = "replica";
        app_info_2.is_stateful = true;
        app_info_2.max_replica_count = 3;
        app_info_2.partition_count = 16;
    }

    void generate_mock_empty_dir_node(int num)
    {
        while (num > 0) {
            dir_node *node_disk =
                new dir_node(fmt::format("tag_empty_{}", num), fmt::format("./tag_empty_{}", num));
            stub->_fs_manager._dir_nodes.emplace_back(node_disk);
            stub->_options.data_dirs.push_back(node_disk->full_dir);
            utils::filesystem::create_directory(node_disk->full_dir);
            num--;
        }
    }

    void generate_mock_dir_nodes(int num)
    {
        int app_id_1_disk_holding_replica_count =
            app_id_1_primary_count_for_disk + app_id_1_secondary_count_for_disk;
        int app_id_2_disk_holding_replica_count =
            app_id_2_primary_count_for_disk + app_id_2_secondary_count_for_disk;

        int app_id_1_partition_index = 1;
        int app_id_2_partition_index = 1;

        int64_t disk_capacity_mb = num * 100;
        int count = 0;
        while (count++ < num) {
            int64_t disk_available_mb = count * 50;
            int disk_available_ratio =
                static_cast<int>(std::round((double)100 * disk_available_mb / disk_capacity_mb));
            // create one mock dir_node and make sure disk_capacity_mb_ > disk_available_mb_
            dir_node *node_disk = new dir_node("tag_" + std::to_string(count),
                                               "./tag_" + std::to_string(count),
                                               disk_capacity_mb,
                                               disk_available_mb,
                                               disk_available_ratio);

            stub->_options.data_dirs.push_back(
                node_disk->full_dir); // open replica need the options
            utils::filesystem::create_directory(node_disk->full_dir);

            int app_1_replica_count_per_disk = app_id_1_disk_holding_replica_count;
            while (app_1_replica_count_per_disk-- > 0) {
                node_disk->holding_replicas[app_info_1.app_id].emplace(
                    gpid(app_info_1.app_id, app_id_1_partition_index++));
            }

            int app_2_replica_count_per_disk = app_id_2_disk_holding_replica_count;
            while (app_2_replica_count_per_disk-- > 0) {
                node_disk->holding_replicas[app_info_2.app_id].emplace(
                    gpid(app_info_2.app_id, app_id_2_partition_index++));
            }

            stub->_fs_manager._dir_nodes.emplace_back(node_disk);
        }
    }
};

} // namespace replication
} // namespace dsn