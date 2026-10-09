#include "platform.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <memory>
#include <mutex>
#include "tracing.h"

static platform::PendingFile g_pending;
static bool g_has_pending = false;
static std::mutex g_dialog_mutex;
static std::string g_save_message;

static void file_dialog_callback(void* /*userdata*/, const char* const* filelist, int /*filter*/) {
    if (filelist && filelist[0]) {
        platform::handle_file_drop(filelist[0]);
    }
}

struct SaveRequest {
    std::string name;
    std::string content;
};

static void save_dialog_callback(void* userdata, const char* const* filelist, int /*filter*/) {
    std::unique_ptr<SaveRequest> request(static_cast<SaveRequest*>(userdata));
    std::string message = filelist ? "Save canceled" : std::string("Save dialog failed: ") + SDL_GetError();
    if (filelist && filelist[0]) {
        SDL_IOStream* io = SDL_IOFromFile(filelist[0], "wb");
        bool ok = false;
        if (io) {
            bool written = SDL_WriteIO(io, request->content.data(), request->content.size()) == request->content.size();
            bool closed = SDL_CloseIO(io);
            ok = written && closed;
        }
        message = ok ? std::string("Saved: ") + filelist[0]
                     : std::string("Could not save ") + filelist[0] + ": " + SDL_GetError();
    }
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_save_message = std::move(message);
}

std::string platform::take_save_message() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    std::string message;
    message.swap(g_save_message);
    return message;
}

void platform::set_gl_attributes() {
    TRACE_FUNCTION_CAT("platform");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
}

const char* platform::glsl_version() {
    return "#version 330 core";
}

float platform::default_font_scale() {
    return 1.4f;
}

const char* platform::ini_filename() {
    return "trace_render.ini";
}

void platform::run_main_loop(void (*step)(), bool* running) {
    while (*running) {
        step();
    }
}

std::string platform::settings_path() {
    TRACE_FUNCTION_CAT("platform");
    std::string dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        dir = std::string(xdg) + "/trace-render";
    } else if (const char* home = std::getenv("HOME")) {
        dir = std::string(home) + "/.config/trace-render";
    } else {
        dir = ".";
    }
    return dir + "/settings.json";
}

bool platform::supports_vsync() {
    return true;
}

void platform::open_file_dialog(SDL_Window* window) {
    TRACE_FUNCTION_CAT("platform");
    if (!window) return;
    static const SDL_DialogFileFilter filters[] = {
        {"Profiles and traces", "diagsession;trprofile;json"},
        {"Visual Studio diagnostics", "diagsession"},
        {"TraceRender profiles", "trprofile"},
        {"JSON Trace Files", "json"},
        {"All Files", "*"},
    };
    SDL_ShowOpenFileDialog(file_dialog_callback, nullptr, window, filters, 5, nullptr, false);
}

void platform::save_file_dialog(SDL_Window* window, const std::string& default_name, const std::string& content) {
    if (!window) return;
    static const SDL_DialogFileFilter filters[] = {
        {"TraceRender Profiles", "trprofile"},
        {"CSV Files", "csv"},
        {"TSV Files", "tsv"},
        {"All Files", "*"},
    };
    auto* request = new SaveRequest{default_name, content};
    bool profile = default_name.size() >= 10 && default_name.substr(default_name.size() - 10) == ".trprofile";
    SDL_ShowSaveFileDialog(save_dialog_callback, request, window, profile ? filters : filters + 1, profile ? 4 : 3,
                           request->name.c_str());
}

void platform::handle_file_drop(const char* path) {
    TRACE_FUNCTION_CAT("platform");
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_pending.path = path;
    g_pending.data.clear();

    // Extract display name
    std::string p(path);
    auto pos = p.find_last_of("/\\");
    g_pending.name = (pos != std::string::npos) ? p.substr(pos + 1) : p;

    g_has_pending = true;
}

bool platform::has_pending_file() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    return g_has_pending;
}

platform::PendingFile platform::take_pending_file() {
    TRACE_FUNCTION_CAT("platform");
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_has_pending = false;
    return std::move(g_pending);
}
