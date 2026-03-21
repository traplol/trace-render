# src/parser/
Chrome JSON trace format parser (simdjson On-Demand, nlohmann for metadata/counters).

## trace_parser.h / trace_parser.cpp — parses file or buffer into `TraceModel`, then calls `build_index()`
```
bool parse(const std::string& filepath, TraceModel& model);
bool parse_buffer(const char* data, size_t size, TraceModel& model);
const std::function<void(const char*, float)>& on_progress() const;
void set_on_progress(std::function<void(const char*, float)> cb);
const std::string& error_message() const;
bool time_unit_ns() const;
void set_time_unit_ns(bool ns);
```
