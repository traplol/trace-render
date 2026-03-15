# src/parser/
Chrome JSON trace format parser (SAX streaming, no full DOM).

## trace_parser.h / trace_parser.cpp — parses file or buffer into `TraceModel`, then calls `build_index()`
```
bool parse(const std::string& filepath, TraceModel& model);
bool parse_buffer(const char* data, size_t size, TraceModel& model);
std::function<void(const char* phase, float progress)> on_progress;
std::string error_message;
bool time_unit_ns = false;
```
