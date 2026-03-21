#include "platform.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include "tracing.h"

static platform::PendingFile g_pending;
static bool g_has_pending = false;

static void file_dialog_callback(void* /*userdata*/, const char* const* filelist, int /*filter*/) {
    if (filelist && filelist[0]) {
        platform::handle_file_drop(filelist[0]);
    }
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
        {"JSON Trace Files", "json"},
        {"All Files", "*"},
    };
    SDL_ShowOpenFileDialog(file_dialog_callback, nullptr, window, filters, 2, nullptr, false);
}

void platform::handle_file_drop(const char* path) {
    TRACE_FUNCTION_CAT("platform");
    g_pending.path = path;
    g_pending.data.clear();

    // Extract display name
    std::string p(path);
    auto pos = p.find_last_of("/\\");
    g_pending.name = (pos != std::string::npos) ? p.substr(pos + 1) : p;

    g_has_pending = true;
}

bool platform::has_pending_file() {
    return g_has_pending;
}

platform::PendingFile platform::take_pending_file() {
    TRACE_FUNCTION_CAT("platform");
    g_has_pending = false;
    return std::move(g_pending);
}
