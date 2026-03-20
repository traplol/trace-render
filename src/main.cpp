#include "app.h"
#include "platform/platform.h"
#include "platform/memory.h"
#include "tracing.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <cstring>

static SDL_Window* g_window = nullptr;
static App* g_app = nullptr;
static bool g_running = true;

static void main_loop_step() {
    TRACE_SCOPE("Frame");
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            g_running = false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(g_window)) {
            g_running = false;
        }
        if (event.type == SDL_EVENT_DROP_FILE) {
            const char* file = event.drop.data;
            if (file) {
                platform::handle_file_drop(file);
            }
        }
    }

    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

    {
        TRACE_SCOPE_CAT("NewFrame", "imgui");
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    {
        TRACE_SCOPE_CAT("AppUpdate", "app");
        g_app->update();
    }

    {
        TRACE_SCOPE_CAT("Render", "imgui");
        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSizeInPixels(g_window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    {
        TRACE_SCOPE_CAT("SwapBuffers", "gl");
        SDL_GL_SwapWindow(g_window);
    }

    // Write performance counter events every frame
    if (Tracer::instance().enabled()) {
        ImGuiIO& io = ImGui::GetIO();
        uint64_t now_us = Tracer::instance().now_us();
        Tracer::instance().write_counter("FPS", "perf", now_us, "fps", (double)io.Framerate);
        Tracer::instance().write_counter("Memory (MB)", "perf", now_us, "rss_mb", get_rss_bytes() / (1024.0 * 1024.0));
    }
}

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    platform::set_gl_attributes();

    SDL_Window* window =
        SDL_CreateWindow("TraceRender", 1600, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = platform::ini_filename();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.ScrollbarRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);

    io.Fonts->AddFontDefault();
    io.FontGlobalScale = platform::default_font_scale();

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(platform::glsl_version());

    App app;
    app.init(window);

    g_window = window;
    g_app = &app;

    // Parse command-line arguments
    const char* file_arg = nullptr;
    const char* trace_output = nullptr;
    bool verbose_trace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ns") == 0) {
            app.set_time_unit_ns(true);
        } else if (strcmp(argv[i], "-us") == 0) {
            app.set_time_unit_ns(false);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_output = argv[++i];
        } else if (strcmp(argv[i], "--verbose-trace") == 0) {
            verbose_trace = true;
        } else {
            file_arg = argv[i];
        }
    }

    if (trace_output) {
        Tracer::instance().set_output(trace_output);
        if (verbose_trace) {
            Tracer::instance().set_verbose(true);
            printf("Verbose tracing to: %s\n", trace_output);
        } else {
            printf("Tracing to: %s\n", trace_output);
        }
    }

    if (file_arg) {
        app.open_file(file_arg);
    }

    // On desktop this loops until g_running is false.
    // On WASM this registers a callback and never returns.
    platform::run_main_loop(main_loop_step, &g_running);

    // Cleanup (only reached on desktop)
    Tracer::instance().close();
    app.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
