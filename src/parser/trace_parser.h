#pragma once
#include "model/trace_model.h"
#include <string>
#include <functional>

class TraceParser {
public:
    bool parse(const std::string& filepath, TraceModel& model);
    bool parse_buffer(const char* data, size_t size, TraceModel& model);

    // Progress callback: receives (phase, progress 0-1)
    const std::function<void(const char* phase, float progress)>& on_progress() const { return on_progress_; }
    void set_on_progress(std::function<void(const char* phase, float progress)> cb) { on_progress_ = std::move(cb); }

    const std::string& error_message() const { return error_message_; }

    bool time_unit_ns() const { return time_unit_ns_; }
    void set_time_unit_ns(bool ns) { time_unit_ns_ = ns; }

private:
    std::function<void(const char* phase, float progress)> on_progress_;
    std::string error_message_;
    bool time_unit_ns_ = false;
};
