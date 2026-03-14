# src/platform/

Platform abstraction layer. Isolates desktop (SDL3 + native file dialog) and WebAssembly (Emscripten) differences behind a common interface. Covers GL context setup, the main loop, settings persistence, file dialogs, and memory monitoring.

## Files

### `platform.h`
Declares the `platform::` namespace interface. Implemented separately by `platform_desktop.cpp` and `platform_wasm.cpp`.

```cpp
namespace platform {

struct PendingFile {
    std::string name;       // display filename
    std::string path;       // non-empty on desktop (file path)
    std::vector<char> data; // non-empty on wasm (file contents)
};

void set_gl_attributes();
const char* glsl_version();

float default_font_scale();
const char* ini_filename();       // nullptr = no ImGui .ini persistence

void run_main_loop(void (*step)(), bool* running);

std::string settings_path();      // empty = no persistence
bool supports_vsync();

void open_file_dialog(SDL_Window* window);
void handle_file_drop(const char* path);

bool has_pending_file();
PendingFile take_pending_file();

} // namespace platform
```

---

### `platform_desktop.cpp`
Desktop implementation of `platform.h`. Uses SDL3 native file dialogs and reads/writes settings to a JSON file in the OS config directory.

---

### `platform_wasm.cpp`
WebAssembly/Emscripten implementation of `platform.h`. Uses the browser file-input API (`EM_ASM`) to receive file data as an in-memory buffer. Settings are not persisted. Main loop is driven by `emscripten_set_main_loop`.

---

### `file_loader.h` / `file_loader_desktop.cpp` / `file_loader_wasm.cpp`
Async file/buffer loader. Spawns a background thread (desktop) or uses synchronous parsing (wasm) to parse a trace file into a `TraceModel` and optionally populate a `QueryDb`. Provides polling, progress reporting, and model ownership transfer.

```cpp
class FileLoader {
    FileLoader();
    ~FileLoader();

    void load_file(const std::string& path, bool time_ns,
                   QueryDb* query_db = nullptr);
    void load_buffer(std::vector<char> data, const std::string& filename,
                     bool time_ns, QueryDb* query_db = nullptr);

    bool is_loading() const;
    bool poll_finished();   // returns true once on completion
    void join();            // block until done

    bool success() const;
    const std::string& error() const;
    const std::string& filename() const;
    float progress() const;
    float phase_progress() const;
    std::string phase() const;

    TraceModel take_model();
};
```

---

### `memory.h`
Inline helper to read the current process RSS (resident set size) in bytes. Linux reads `/proc/self/statm`; macOS uses `task_info`; other platforms return 0.

```cpp
size_t get_rss_bytes();
```
