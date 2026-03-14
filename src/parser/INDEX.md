# src/parser/

Trace file parsing. Reads Chrome JSON trace format files (both array-of-events and object-with-traceEvents forms) and populates a `TraceModel`. Parsing is done with a streaming SAX handler to avoid loading the full JSON DOM into memory.

## Files

### `trace_parser.h` / `trace_parser.cpp`
Parses a Chrome-format JSON trace from a file path or an in-memory buffer. After parsing raw events, it delegates to `TraceModel::build_index()` to sort, depth-assign, and spatially index the data. Progress and errors are surfaced via public fields.

```cpp
class TraceParser {
    bool parse(const std::string& filepath, TraceModel& model);
    bool parse_buffer(const char* data, size_t size, TraceModel& model);

    std::function<void(const char* phase, float progress)> on_progress;
    std::string error_message;
    bool time_unit_ns;  // divide timestamps by 1000 when true
};
```
