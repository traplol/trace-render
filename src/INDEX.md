# src/
Top-level app shell and self-tracing helpers. Subdirs: `model/` `parser/` `platform/` `ui/`

## app.h — `App`: owns all subsystems, drives the frame loop
```
void init(SDL_Window*); void update(); void shutdown();
void open_file(const std::string&); void open_buffer(std::vector<char>, const std::string&);
void set_time_unit_ns(bool); bool has_trace() const;
```

## tracing.h — `Tracer` singleton + RAII scopes for Chrome-format JSON trace output
```
// Tracer
static Tracer& instance();
void set_output(const std::string& path); void close(); bool enabled() const;
void write_complete(const char* name, const char* cat, uint64_t ts_us, uint64_t dur_us, const char* args_json = nullptr);
void write_counter(const char* name, const char* cat, uint64_t ts_us, const char* key, double value);
uint64_t now_us() const;
// Macros
TRACE_SCOPE(name)  TRACE_SCOPE_CAT(name, cat)  TRACE_SCOPE_ARGS(name, cat, ...)
TRACE_FUNCTION()   TRACE_FUNCTION_CAT(cat)
```
