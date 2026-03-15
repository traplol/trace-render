# src/platform/
Platform abstraction: GL setup, main loop, file dialogs, async loading, memory monitoring.
Desktop impl: `platform_desktop.cpp` / `file_loader_desktop.cpp`. Wasm impl: `platform_wasm.cpp` / `file_loader_wasm.cpp`.

## platform.h — `platform::` namespace; implemented per-platform
```
// PendingFile fields: name, path (desktop), data (wasm)
void set_gl_attributes();
const char* glsl_version();
float default_font_scale();
const char* ini_filename();
void run_main_loop(void (*step)(), bool* running);
std::string settings_path();
bool supports_vsync();
void open_file_dialog(SDL_Window*);
void handle_file_drop(const char* path);
bool has_pending_file();
PendingFile take_pending_file();
```

## file_loader.h — async trace loader; background thread (desktop) or sync (wasm)
```
void load_file(const std::string& path, bool time_ns, QueryDb* query_db = nullptr);
void load_buffer(std::vector<char> data, const std::string& filename, bool time_ns, QueryDb* query_db = nullptr);
bool is_loading() const;
bool poll_finished();
void join();
bool success() const;
const std::string& error() const;
const std::string& filename() const;
float progress() const;
float phase_progress() const;
std::string phase() const;
TraceModel take_model();
```

## memory.h — RSS query (Linux: `/proc/self/statm`, macOS: `task_info`, others: 0)
```
size_t get_rss_bytes();
```
