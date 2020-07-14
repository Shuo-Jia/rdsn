// Copyright (c) 2017-present, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#pragma once
#include <dsn/service_api_c.h>
#include <dsn/dist/fmt_logging.h>

namespace dsn {
namespace tool {

/**
 * latency_tracer is a simple tool for tracking request time consuming in different stages, which
 * can help user find the latency bottleneck. user needs to use it to "add_point" in one stage,
 * which will record the name of point and the time_used. when the request is finshed, you can dump
 * the formated result.
 *
 * for example: one request experiences four stages, latency_tracer need be held by request and
 * passes all stages:
 * class request {
 *      latency_tracer tracer
 * }
 * void start(request req){
 *      req.tracer.add_point("start", now);
 * };
 * void stageA(request req){
 *      req.tracer.add_point("stageA", now);
 * };
 * void stageB(request req){
 *      req.tracer.add_point("stageB", now, true);
 * };
 * void end(request req){
 *      req.tracer.add_point("end", now);
 * };
 *
 *  point1     point2     point3    point4
 *    |         |           |         |
 *    |         |           |         |
 *  start---->stageA----->stageB---->end
 *
 * the "points" will record the all points' time_used_from_previous and time_used_from_start
**/

struct latency_tracer
{

public:
    uint64_t id;
    std::string type;
    std::map<int64_t, std::string> points;

public:
    latency_tracer(int id, const std::string &start_name, const std::string &type)
        : id(id), type(type)
    {
        add_point(start_name);
    };

    // this method is called for any other method which will be recorded methed name and ts
    //
    // -name: generally, it is the name of that call this method. but you can define the more
    // significant name to show the events of one moment
    // -ts: current timestamp
    void add_point(const std::string &name, int64_t ts = dsn_now_ns()) { points.emplace(ts, name); }

    void dump_trace_points(int threshold)
    {
        if (threshold <= 0) {
            return;
        }

        int64_t start_time = points.begin()->first;

        if (points.end()->first - start_time < threshold) {
            return;
        }

        int64_t previous_time = points.end()->first;
        std::string trace;
        for (const auto &point : points) {
            trace =
                fmt::format("{}\tTRACER[{:<10}|{:<10}]:from_previous={:<20}, from_start={:<20}, "
                            "ts={:<20}, name={:<20}\n",
                            trace,
                            type,
                            id,
                            point.first - previous_time,
                            point.first - start_time,
                            point.first,
                            point.second);
            previous_time = point.first;
        }

        derror_f("TRACE:the request excceed {}\n{}", threshold, trace);
    }
};
} // namespace tool
} // namespace dsn