# src/

Top-level application source. Contains the main application class, entry point, and the self-contained performance tracing infrastructure. Subdirectories are organized by layer: `model/`, `parser/`, `platform/`, and `ui/`.

## Files

### `app.h` / `app.cpp`
The central application class. Owns all subsystems (model, view state, UI panels, file loader, query DB) and drives the per-frame update loop. Handles file open, loading overlay, settings modal, and theme/vsync persistence.

```cpp
class App {
    void init(SDL_Window* window);
    void update();
    void shutdown();
    void open_file(const std::string& path);
    void open_buffer(std::vector<char> data, const std::string& filename);
    void set_time_unit_ns(bool ns);
    bool has_trace() const;
};
```

---

### `main.cpp`
Entry point. Initialises SDL3, creates the OpenGL context, sets up ImGui, then hands off to `App` and the platform main loop.

---

### `tracing.h`
Self-contained Chrome-trace-format performance tracing infrastructure. Provides a singleton `Tracer`, two RAII scope classes, and convenience macros. All output is written as JSON to a file opened via `set_output()`.

```cpp
// Singleton tracer
class Tracer {
    static Tracer& instance();
    void set_output(const std::string& path);
    void close();
    bool enabled() const;
    void write_complete(const char* name, const char* cat, uint64_t ts_us, uint64_t dur_us,
                        const char* args_json = nullptr);
    void write_counter(const char* name, const char* cat, uint64_t ts_us,
                       const char* key, double value);
    uint64_t now_us() const;
};

// RAII scope — emits file/line/func as args automatically
class TraceScope {
    TraceScope(const char* name, const char* cat,
               const char* file, int line, const char* func);
};

// RAII scope with additional user-supplied key/value args
class TraceScopeArgs {
    TraceScopeArgs(const char* name, const char* cat,
                   const char* file, int line, const char* func,
                   std::string extra_args);
};

// Macros
TRACE_SCOPE(name)
TRACE_SCOPE_CAT(name, cat)
TRACE_SCOPE_ARGS(name, cat, ...)
TRACE_FUNCTION()
TRACE_FUNCTION_CAT(cat)
```

## Subdirectories

| Directory | Description |
|-----------|-------------|
| `model/`    | Core data structures: events, trace model, block index, query DB, color palette |
| `parser/`   | Chrome JSON trace file parser |
| `platform/` | Platform abstraction: GL setup, file dialogs, file loading, memory monitoring |
| `ui/`       | All ImGui panels and rendering components |
