#pragma once
#include "model/trace_model.h"
#include <string>
#include <functional>

class TraceParser {
public:
    bool parse(const std::string& filepath, TraceModel& model);
    bool parse_buffer(const char* data, size_t size, TraceModel& model);

    // Progress callback: receives (phase, progress 0-1)
    // phase: "Reading file", "Parsing JSON", "Building index"
    std::function<void(const char* phase, float progress)> on_progress;
    std::string error_message;

    // When true, JSON timestamps are in nanoseconds and will be divided by 1000
    bool time_unit_ns = false;
};
