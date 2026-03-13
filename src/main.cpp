#include "app.h"
#include "tracing.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

// Globals needed for the main loop callback in Emscripten
static SDL_Window* g_window = nullptr;
static SDL_GLContext g_gl_context = nullptr;
static App* g_app = nullptr;
static bool g_running = true;

static void main_loop_step() {
    ImGuiIO& io = ImGui::GetIO();

    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                g_running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(g_window)) {
                g_running = false;
            }
            // Handle file drop
            if (event.type == SDL_EVENT_DROP_FILE) {
                const char* file = event.drop.data;
                if (file) {
                    g_app->open_file(file);
                }
            }
        }
    }

    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    g_app->update();

    ImGui::Render();
    int display_w, display_h;
    SDL_GetWindowSizeInPixels(g_window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(g_window);
}

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __EMSCRIPTEN__
    // WebGL 2.0 = OpenGL ES 3.0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // GL 3.3 Core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* window = SDL_CreateWindow("Perfetto Trace Viewer", 1600, 900,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

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
    SDL_GL_SetSwapInterval(1);  // vsync

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#ifdef __EMSCRIPTEN__
    io.IniFilename = nullptr;  // No .ini persistence in browser
#else
    io.IniFilename = "perfetto_imgui.ini";
#endif

    // Dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.ScrollbarRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);

    // Scale up font
    io.Fonts->AddFontDefault();
#ifdef __EMSCRIPTEN__
    io.FontGlobalScale = 1.5f;  // Lower scale for browser (already high DPI)
#else
    io.FontGlobalScale = 3.0f;
#endif

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
#ifdef __EMSCRIPTEN__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif

    App app;
    app.init(window);

    g_window = window;
    g_gl_context = gl_context;
    g_app = &app;

#ifndef __EMSCRIPTEN__
    // Parse command-line arguments
    const char* file_arg = nullptr;
    const char* trace_output = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ns") == 0) {
            app.set_time_unit_ns(true);
        } else if (strcmp(argv[i], "-us") == 0) {
            app.set_time_unit_ns(false);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_output = argv[++i];
        } else {
            file_arg = argv[i];
        }
    }

    if (trace_output) {
        Tracer::instance().set_output(trace_output);
        printf("Tracing to: %s\n", trace_output);
    }

    if (file_arg) {
        app.open_file(file_arg);
    }

    while (g_running) {
        TRACE_SCOPE("Frame");
        main_loop_step();

        // Write FPS counter event every frame
        if (Tracer::instance().enabled()) {
            Tracer::instance().write_counter("FPS", "perf", Tracer::instance().now_us(), "fps", (double)io.Framerate);
        }
    }

    Tracer::instance().close();
    app.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
#else
    emscripten_set_main_loop(main_loop_step, 0, true);
#endif

    return 0;
}
