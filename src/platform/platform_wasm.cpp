#include "platform.h"
#include <SDL3/SDL.h>
#include <emscripten.h>

static platform::PendingFile g_pending;
static bool g_has_pending = false;

extern "C" {
EMSCRIPTEN_KEEPALIVE
void wasm_file_loaded(const char* filename, const char* data, unsigned int size) {
    g_pending.name = filename;
    g_pending.path.clear();
    g_pending.data.assign(data, data + size);
    g_has_pending = true;
}
}

static void trigger_file_input() {
    EM_ASM({
        var input = document.getElementById('file-input');
        if (!input) {
            input = document.createElement('input');
            input.type = 'file';
            input.id = 'file-input';
            input.accept = '.json';
            input.style.display = 'none';
            document.body.appendChild(input);
            input.addEventListener(
                'change', function(e) {
                    var file = e.target.files[0];
                    if (file) loadFileToWasm(file);
                    input.value = null;
                });
        }
        input.click();
    });
}

void platform::set_gl_attributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
}

const char* platform::glsl_version() {
    return "#version 300 es";
}

float platform::default_font_scale() {
    return 1.5f;
}

const char* platform::ini_filename() {
    return nullptr;
}

void platform::run_main_loop(void (*step)(), bool* /*running*/) {
    emscripten_set_main_loop(step, 0, true);
}

std::string platform::settings_path() {
    return "";
}

bool platform::supports_vsync() {
    return false;
}

void platform::open_file_dialog(SDL_Window* /*window*/) {
    trigger_file_input();
}

void platform::handle_file_drop(const char* /*path*/) {
    // No-op: drops handled via JS in shell.html
}

bool platform::has_pending_file() {
    return g_has_pending;
}

platform::PendingFile platform::take_pending_file() {
    g_has_pending = false;
    return std::move(g_pending);
}
