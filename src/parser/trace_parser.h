#pragma once
#include "model/trace_model.h"
#include <string>
#include <functional>

class TraceParser {
public:
    bool parse(const std::string& filepath, TraceModel& model);

    std::function<void(float)> on_progress;
    std::string error_message;

    // When true, JSON timestamps are in nanoseconds and will be divided by 1000
    bool time_unit_ns = false;
};
