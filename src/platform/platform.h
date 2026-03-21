#pragma once
#include <string>
#include <vector>

struct SDL_Window;

namespace platform {

struct PendingFile {
    std::string name;        // display filename
    std::string path;        // non-empty for file path (desktop)
    std::vector<char> data;  // non-empty for buffer data (wasm)
};

// GL context setup
void set_gl_attributes();
const char* glsl_version();

// Display defaults
float default_font_scale();
const char* ini_filename();  // nullptr = no persistence

// Main loop control
void run_main_loop(void (*step)(), bool* running);

// Settings persistence
std::string settings_path();  // empty = no persistence
bool supports_vsync();

// File dialogs
void open_file_dialog(SDL_Window* window);
void save_file_dialog(SDL_Window* window, const std::string& default_name, const std::string& content);

// File drop handling (called from SDL event loop)
void handle_file_drop(const char* path);

// Pending file from dialog or drag-and-drop
bool has_pending_file();
PendingFile take_pending_file();

}  // namespace platform
