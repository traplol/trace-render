# src/parser/
Chrome JSON streaming import and versioned TraceRender profile import/export.

## trace_parser.h / trace_parser.cpp — dispatches saved profiles and streams Chrome events, stack definitions, and sampling weights into `TraceModel`
```
bool parse(const std::string& filepath, TraceModel& model);
bool parse_buffer(const char* data, size_t size, TraceModel& model);
const std::function<void(const char*, float)>& on_progress() const;
void set_on_progress(std::function<void(const char*, float)> cb);
const std::string& error_message() const;
bool time_unit_ns() const;
void set_time_unit_ns(bool ns);
```

## profile_io.h / profile_io.cpp — reads and writes version 1 profiles with validated identities, CPU observations, allocation lifetimes, managed observations, and capture metadata
```
bool read_profile(std::string_view data, TraceModel& model, std::string& error);
bool serialize_profile(const TraceModel& model, std::string& data, std::string& error);
bool write_profile(const std::string& filepath, const TraceModel& model, std::string& error);
```
