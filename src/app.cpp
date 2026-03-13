#include "app.h"
#include "tracing.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>

void App::init(SDL_Window* window) {
    TRACE_FUNCTION_CAT("app");
    window_ = window;
    toolbar_.set_window(window);
#ifndef __EMSCRIPTEN__
    load_settings();
    SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
#endif
}

void App::shutdown() {
    TRACE_FUNCTION_CAT("app");
#ifndef __EMSCRIPTEN__
    if (load_thread_.joinable()) load_thread_.join();
#endif
    save_settings();
}

#ifdef __EMSCRIPTEN__

void App::open_file(const std::string& path) {
    // In WASM, open_file loads synchronously (no threads)
    if (loading_) return;

    loading_filename_ = path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) loading_filename_ = path.substr(pos + 1);

    status_message_ = "Loading: " + loading_filename_;

    TraceParser parser;
    parser.time_unit_ns = view_.time_unit_ns;

    TraceModel new_model;
    bool ok = parser.parse(path, new_model);

    if (ok) {
        model_ = std::move(new_model);
        query_db_.load(model_);
        has_trace_ = true;
        view_.view_start_ts = 0.0;
        view_.view_end_ts = 1000.0;
        view_.selected_event_idx = -1;
        view_.hidden_pids.clear();
        view_.hidden_tids.clear();
        view_.hidden_cats.clear();
        view_.search_query.clear();
        view_.search_results.clear();
        view_.search_current = -1;
        if (model_.min_ts_ < model_.max_ts_) {
            view_.zoom_to_fit(model_.min_ts_, model_.max_ts_);
        }
        status_message_ = "Loaded: " + loading_filename_ + " (" + std::to_string(model_.events_.size()) + " events, " +
                          std::to_string(model_.processes_.size()) + " processes)";
    } else {
        status_message_ = "Error: " + parser.error_message;
        has_trace_ = false;
    }
}

void App::open_buffer(const char* data, size_t size, const std::string& filename) {
    if (loading_) return;

    loading_filename_ = filename;
    status_message_ = "Loading: " + loading_filename_;

    TraceParser parser;
    parser.time_unit_ns = view_.time_unit_ns;

    TraceModel new_model;
    bool ok = parser.parse_buffer(data, size, new_model);

    if (ok) {
        model_ = std::move(new_model);
        query_db_.load(model_);
        has_trace_ = true;
        view_.view_start_ts = 0.0;
        view_.view_end_ts = 1000.0;
        view_.selected_event_idx = -1;
        view_.hidden_pids.clear();
        view_.hidden_tids.clear();
        view_.hidden_cats.clear();
        view_.search_query.clear();
        view_.search_results.clear();
        view_.search_current = -1;
        if (model_.min_ts_ < model_.max_ts_) {
            view_.zoom_to_fit(model_.min_ts_, model_.max_ts_);
        }
        status_message_ = "Loaded: " + loading_filename_ + " (" + std::to_string(model_.events_.size()) + " events, " +
                          std::to_string(model_.processes_.size()) + " processes)";
    } else {
        status_message_ = "Error: " + parser.error_message;
        has_trace_ = false;
    }
}

#else  // !__EMSCRIPTEN__

void App::open_buffer(const char* data, size_t size, const std::string& filename) {
    (void)data;
    (void)size;
    (void)filename;
    // Desktop uses open_file with filesystem paths
}

void App::open_file(const std::string& path) {
    // Don't start a new load while one is in progress
    if (loading_) return;

    // Join any previous load thread
    if (load_thread_.joinable()) load_thread_.join();

    // Extract filename for display
    loading_filename_ = path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) loading_filename_ = path.substr(pos + 1);

    status_message_ = "Loading: " + loading_filename_;
    loading_ = true;
    load_progress_ = 0.0f;
    load_phase_progress_ = 0.0f;
    load_finished_ = false;
    load_success_ = false;
    load_error_.clear();

    bool time_ns = view_.time_unit_ns;

    load_thread_ = std::thread([this, path, time_ns]() {
        TRACE_SCOPE_CAT("OpenFile", "io");

        TraceParser parser;
        // Phase weights for global progress: Reading 0-25%, Parsing 25-60%, Building index 60-80%
        parser.on_progress = [this](const char* phase, float p) {
            {
                std::lock_guard<std::mutex> lock(phase_mutex_);
                loading_phase_ = phase;
            }
            load_phase_progress_.store(p, std::memory_order_relaxed);

            float global = 0.0f;
            if (std::string_view(phase) == "Reading file") {
                global = p * 0.25f;
            } else if (std::string_view(phase) == "Parsing JSON") {
                global = 0.25f + p * 0.35f;
            } else if (std::string_view(phase) == "Building index") {
                global = 0.60f + p * 0.20f;
            } else {
                global = 0.80f + p * 0.10f;
            }
            load_progress_.store(global, std::memory_order_relaxed);
        };
        parser.time_unit_ns = time_ns;

        TraceModel new_model;
        bool ok = parser.parse(path, new_model);

        if (ok) {
            // Build query DB on background thread too
            {
                std::lock_guard<std::mutex> lock(phase_mutex_);
                loading_phase_ = "Building query DB";
            }
            load_phase_progress_.store(0.0f, std::memory_order_relaxed);
            load_progress_.store(0.90f, std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lock(load_mutex_);
        if (ok) {
            model_ = std::move(new_model);
            query_db_.load(model_);
            load_success_ = true;
        } else {
            load_success_ = false;
            load_error_ = parser.error_message;
        }
        load_finished_ = true;
    });
}

#endif  // __EMSCRIPTEN__

#ifndef __EMSCRIPTEN__

void App::finish_load() {
    TRACE_FUNCTION_CAT("app");
    if (load_thread_.joinable()) load_thread_.join();

    std::lock_guard<std::mutex> lock(load_mutex_);

    if (load_success_) {
        has_trace_ = true;
        view_.view_start_ts = 0.0;
        view_.view_end_ts = 1000.0;
        view_.selected_event_idx = -1;
        view_.hidden_pids.clear();
        view_.hidden_tids.clear();
        view_.hidden_cats.clear();
        view_.search_query.clear();
        view_.search_results.clear();
        view_.search_current = -1;
        if (model_.min_ts_ < model_.max_ts_) {
            view_.zoom_to_fit(model_.min_ts_, model_.max_ts_);
        }
        status_message_ = "Loaded: " + loading_filename_ + " (" + std::to_string(model_.events_.size()) + " events, " +
                          std::to_string(model_.processes_.size()) + " processes)";
    } else {
        status_message_ = "Error: " + load_error_;
        has_trace_ = false;
    }

    loading_ = false;
    load_finished_ = false;
}

#endif  // !__EMSCRIPTEN__

void App::render_loading_overlay() {
    TRACE_FUNCTION_CAT("ui");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 center = vp->GetCenter();

    // Semi-transparent fullscreen overlay
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
    ImGui::Begin("##LoadingOverlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleColor();

#ifdef __EMSCRIPTEN__
    float progress = load_progress_;
    std::string phase = loading_phase_;
#else
    float progress = load_progress_.load(std::memory_order_relaxed);
    std::string phase;
    {
        std::lock_guard<std::mutex> lock(phase_mutex_);
        phase = loading_phase_;
    }
#endif

    // Center the loading content
    float content_w = 600.0f;
    float content_h = 160.0f;
    ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - content_w) / 2, (vp->WorkSize.y - content_h) / 2));

    ImGui::BeginGroup();

    // Spinner + filename
    float time = (float)ImGui::GetTime();
    const char* spinner_frames[] = {"|", "/", "-", "\\"};
    int frame = (int)(time * 4.0f) % 4;

    char loading_text[256];
    snprintf(loading_text, sizeof(loading_text), "%s  Loading %s", spinner_frames[frame], loading_filename_.c_str());
    ImVec2 text_size = ImGui::CalcTextSize(loading_text);
    ImGui::SetCursorPosX((vp->WorkSize.x - text_size.x) / 2);
    ImGui::Text("%s", loading_text);

    ImGui::Spacing();

    // Phase text
    if (!phase.empty()) {
        ImVec2 phase_size = ImGui::CalcTextSize(phase.c_str());
        ImGui::SetCursorPosX((vp->WorkSize.x - phase_size.x) / 2);
        ImGui::TextDisabled("%s", phase.c_str());
    }

    ImGui::Spacing();

    // Global progress bar
    ImGui::SetCursorPosX((vp->WorkSize.x - content_w) / 2);
    ImGui::ProgressBar(progress, ImVec2(content_w, 0));

    ImGui::EndGroup();

    ImGui::End();
}

void App::update() {
    TRACE_SCOPE("App::update");

#ifndef __EMSCRIPTEN__
    // Check if background load is complete
    if (load_finished_.load(std::memory_order_acquire)) {
        finish_load();
    }
#endif

    // Set up dockspace over the entire viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("DockSpaceHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");

    // Build default layout on first frame
    if (first_layout_ && ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        first_layout_ = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        // Split: right panel (25%) for details/filters, remainder for timeline+search
        ImGuiID dock_main = dockspace_id;
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.25f, nullptr, &dock_main);

        // Split right panel: top for details, bottom for filters
        ImGuiID dock_right_bottom = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.5f, nullptr, &dock_right);

        // Split main: bottom (25%) for search, top for timeline
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.25f, nullptr, &dock_main);

        ImGui::DockBuilderDockWindow("Timeline", dock_main);
        ImGui::DockBuilderDockWindow("Details", dock_right);
        ImGui::DockBuilderDockWindow("Filters", dock_right_bottom);
        ImGui::DockBuilderDockWindow("Search", dock_bottom);
        ImGui::DockBuilderDockWindow("Statistics", dock_bottom);
        ImGui::DockBuilderDockWindow("Instances", dock_right_bottom);
        ImGui::DockBuilderDockWindow("Diagnostics", dock_right_bottom);
        ImGui::DockBuilderDockWindow("Source", dock_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
    } else {
        first_layout_ = false;
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();

    // Menu bar / toolbar
    toolbar_.render(model_, view_);
    if (toolbar_.file_open_requested()) {
        open_file(toolbar_.file_path());
        toolbar_.clear_request();
    }
#ifdef __EMSCRIPTEN__
    if (toolbar_.file_data_ready()) {
        open_buffer(toolbar_.file_data().data(), toolbar_.file_data().size(), toolbar_.file_name());
        toolbar_.clear_file_data();
    }
#endif
    if (toolbar_.settings_requested()) {
        show_settings_ = true;
        toolbar_.clear_settings_request();
    }
    // Ctrl+, shortcut
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) {
        show_settings_ = true;
    }
    if (show_settings_) {
        render_settings_modal();
    }

    if (has_trace_ && !loading_) {
        timeline_.render(model_, view_);
        detail_.render(model_, view_);
        search_.render(model_, view_);
        filter_.render(model_, view_);
        stats_.render(model_, query_db_, view_);
        instances_.render(model_, view_);
        diagnostics_.stats = timeline_.diag_stats;
        source_.render(model_, view_);
        diagnostics_.render(model_, view_);
    } else {
        // Welcome screen
        ImGui::Begin("Timeline");
        if (!loading_) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 text_size = ImGui::CalcTextSize("Open a Chrome trace file (Ctrl+O) or drag & drop");
            ImGui::SetCursorPos(ImVec2((avail.x - text_size.x) / 2, (avail.y - text_size.y) / 2));
            ImGui::TextDisabled("Open a Chrome trace file (Ctrl+O) or drag & drop");
        }
        ImGui::End();

        ImGui::Begin("Details");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        ImGui::Begin("Search");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        ImGui::Begin("Filters");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        ImGui::Begin("Statistics");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        ImGui::Begin("Instances");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        ImGui::Begin("Source");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        diagnostics_.render(model_, view_);
    }

    // Loading overlay (drawn on top of everything)
    if (loading_) {
        render_loading_overlay();
    }

    // Status bar
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float status_h = ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - status_h));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, status_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
        ImGui::Begin("##StatusBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%s", status_message_.c_str());
        if (loading_) {
#ifdef __EMSCRIPTEN__
            std::string phase = loading_phase_;
            float phase_progress = load_phase_progress_;
#else
            std::string phase;
            {
                std::lock_guard<std::mutex> lock(phase_mutex_);
                phase = loading_phase_;
            }
            float phase_progress = load_phase_progress_.load(std::memory_order_relaxed);
#endif
            if (!phase.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("%s", phase.c_str());
            }
            ImGui::SameLine();
            ImGui::ProgressBar(phase_progress, ImVec2(150, 0));
        }
        if (has_trace_ && !loading_ && view_.selected_event_idx >= 0) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 900);
            const auto& ev = model_.events_[view_.selected_event_idx];
            ImGui::Text("Selected: %s", model_.get_string(ev.name_idx).c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void App::render_settings_modal() {
    TRACE_FUNCTION_CAT("ui");
    ImGui::OpenPopup("Settings");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(800, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Settings", &show_settings_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SeparatorText("Display");

        float font_scale = ImGui::GetIO().FontGlobalScale;
        if (ImGui::SliderFloat("Font Scale", &font_scale, 0.5f, 6.0f, "%.1f")) {
            ImGui::GetIO().FontGlobalScale = font_scale;
        }

        ImGui::SeparatorText("Timeline Layout");

        ImGui::SliderFloat("Track Height", &view_.track_height, 20.0f, 200.0f, "%.0f px");
        ImGui::SliderFloat("Track Padding", &view_.track_padding, 0.0f, 30.0f, "%.0f px");
        ImGui::SliderFloat("Counter Track Height", &view_.counter_track_height, 60.0f, 400.0f, "%.0f px");
        ImGui::SliderFloat("Label Gutter Width", &view_.label_width, 100.0f, 1200.0f, "%.0f px");
        ImGui::SliderFloat("Ruler Height", &view_.ruler_height, 15.0f, 120.0f, "%.0f px");
        ImGui::SliderFloat("Process Header Height", &view_.proc_header_height, 10.0f, 80.0f, "%.0f px");
        ImGui::SliderFloat("Scrollbar Scale", &view_.scrollbar_scale, 0.5f, 5.0f, "%.1f");

        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Show Flow Arrows", &view_.show_flows);
        ImGui::ColorEdit4("Selection Border Color", view_.sel_border_color,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

#ifndef __EMSCRIPTEN__
        if (ImGui::Checkbox("VSync", &vsync_)) {
            SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
        }
#endif

        ImGui::SeparatorText("Parser");

        ImGui::Checkbox("Interpret timestamps as nanoseconds", &view_.time_unit_ns);
        ImGui::SameLine();
        ImGui::TextDisabled("(reload file to apply)");

        ImGui::SeparatorText("Theme");

        if (ImGui::RadioButton("Dark", dark_theme_)) {
            dark_theme_ = true;
            ImGui::StyleColorsDark();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Light", !dark_theme_)) {
            dark_theme_ = false;
            ImGui::StyleColorsLight();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Close", ImVec2(200, 0))) {
            save_settings();
            show_settings_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

#ifndef __EMSCRIPTEN__
std::string App::settings_path() const {
    std::string dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        dir = std::string(xdg) + "/perfetto-imgui";
    } else if (const char* home = std::getenv("HOME")) {
        dir = std::string(home) + "/.config/perfetto-imgui";
    } else {
        dir = ".";
    }
    return dir + "/settings.json";
}

void App::save_settings() {
    TRACE_FUNCTION_CAT("io");
    std::string path = settings_path();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    nlohmann::json j;
    j["font_scale"] = ImGui::GetIO().FontGlobalScale;
    j["track_height"] = view_.track_height;
    j["track_padding"] = view_.track_padding;
    j["counter_track_height"] = view_.counter_track_height;
    j["label_width"] = view_.label_width;
    j["show_flows"] = view_.show_flows;
    j["time_unit_ns"] = view_.time_unit_ns;
    j["ruler_height"] = view_.ruler_height;
    j["proc_header_height"] = view_.proc_header_height;
    j["scrollbar_scale"] = view_.scrollbar_scale;
    j["sel_border_color"] = {view_.sel_border_color[0], view_.sel_border_color[1], view_.sel_border_color[2],
                             view_.sel_border_color[3]};
    j["dark_theme"] = dark_theme_;
    j["vsync"] = vsync_;
    j["query_tabs"] = stats_.save_tabs();
    j["source_panel"] = source_.save_settings();

    std::ofstream f(path);
    if (f.is_open()) {
        f << j.dump(2);
    }
}

void App::load_settings() {
    TRACE_FUNCTION_CAT("io");
    std::string path = settings_path();
    std::ifstream f(path);
    if (!f.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(f);

        if (j.contains("font_scale")) ImGui::GetIO().FontGlobalScale = j["font_scale"].get<float>();
        if (j.contains("track_height")) view_.track_height = j["track_height"].get<float>();
        if (j.contains("track_padding")) view_.track_padding = j["track_padding"].get<float>();
        if (j.contains("counter_track_height")) view_.counter_track_height = j["counter_track_height"].get<float>();
        if (j.contains("label_width")) view_.label_width = j["label_width"].get<float>();
        if (j.contains("show_flows")) view_.show_flows = j["show_flows"].get<bool>();
        if (j.contains("time_unit_ns")) view_.time_unit_ns = j["time_unit_ns"].get<bool>();
        if (j.contains("ruler_height")) view_.ruler_height = j["ruler_height"].get<float>();
        if (j.contains("proc_header_height")) view_.proc_header_height = j["proc_header_height"].get<float>();
        if (j.contains("scrollbar_scale")) view_.scrollbar_scale = j["scrollbar_scale"].get<float>();
        if (j.contains("sel_border_color")) {
            auto& arr = j["sel_border_color"];
            for (int i = 0; i < 4; i++) view_.sel_border_color[i] = arr[i].get<float>();
        }
        if (j.contains("dark_theme")) {
            dark_theme_ = j["dark_theme"].get<bool>();
            if (dark_theme_)
                ImGui::StyleColorsDark();
            else
                ImGui::StyleColorsLight();
        }
        if (j.contains("vsync")) vsync_ = j["vsync"].get<bool>();
        if (j.contains("query_tabs")) {
            stats_.load_tabs(j["query_tabs"]);
        }
        if (j.contains("source_panel")) {
            source_.load_settings(j["source_panel"]);
        }
    } catch (...) {
        // Ignore malformed settings file
    }
}

#else  // __EMSCRIPTEN__

std::string App::settings_path() const {
    return "";
}
void App::save_settings() {}
void App::load_settings() {}

#endif  // __EMSCRIPTEN__
