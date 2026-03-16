#include "app.h"
#include "platform/platform.h"
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
    load_settings();
    if (platform::supports_vsync()) {
        SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
    }
}

void App::shutdown() {
    TRACE_FUNCTION_CAT("app");
    loader_.join();
    save_settings();
}

void App::open_file(const std::string& path) {
    if (loader_.is_loading()) return;
    loader_.load_file(path, view_.time_unit_ns(), &query_db_);
    status_message_ = "Loading: " + loader_.filename();
}

void App::open_buffer(std::vector<char> data, const std::string& filename) {
    if (loader_.is_loading()) return;
    loader_.load_buffer(std::move(data), filename, view_.time_unit_ns(), &query_db_);
    status_message_ = "Loading: " + loader_.filename();
}

void App::finish_load() {
    TRACE_FUNCTION_CAT("app");
    if (loader_.success()) {
        model_ = loader_.take_model();
        has_trace_ = true;
        view_.set_view_start_ts(0.0);
        view_.set_view_end_ts(1000.0);
        view_.set_selected_event_idx(-1);
        view_.clear_hidden_pids();
        view_.clear_hidden_tids();
        view_.clear_hidden_cats();
        view_.clear_search_query();
        view_.clear_search_results();
        view_.set_search_current(-1);
        if (model_.min_ts() < model_.max_ts()) {
            view_.zoom_to_fit(model_.min_ts(), model_.max_ts());
        }
        status_message_ = "Loaded: " + loader_.filename() + " (" + std::to_string(model_.events().size()) +
                          " events, " + std::to_string(model_.processes().size()) + " processes)";
        query_db_.create_indexes_async();
    } else {
        status_message_ = "Error: " + loader_.error();
        has_trace_ = false;
    }
}

void App::render_loading_overlay() {
    TRACE_FUNCTION_CAT("ui");
    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Semi-transparent fullscreen overlay
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
    ImGui::Begin("##LoadingOverlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleColor();

    float progress = loader_.progress();
    std::string phase = loader_.phase();

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
    snprintf(loading_text, sizeof(loading_text), "%s  Loading %s", spinner_frames[frame], loader_.filename().c_str());
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

    // Check if background load is complete
    if (loader_.poll_finished()) {
        finish_load();
    }

    // Check for pending files from platform (dialog, drag-and-drop)
    if (platform::has_pending_file()) {
        auto f = platform::take_pending_file();
        if (!f.data.empty()) {
            open_buffer(std::move(f.data), f.name);
        } else {
            open_file(f.path);
        }
    }

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

        // Split: left panel (18%) for diagnostics, remainder for main content
        ImGuiID dock_main = dockspace_id;
        ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.18f, nullptr, &dock_main);

        // Split main: bottom (42%) for search/instances, top for timeline/details
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.42f, nullptr, &dock_main);

        // Split top: right (35%) for details/filters, left for timeline/source
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.35f, nullptr, &dock_main);

        // Split bottom: right (37%) for instances, left for search/statistics
        ImGuiID dock_bottom_right =
            ImGui::DockBuilderSplitNode(dock_bottom, ImGuiDir_Right, 0.37f, nullptr, &dock_bottom);

        ImGui::DockBuilderDockWindow("Diagnostics", dock_left);
        ImGui::DockBuilderDockWindow("Timeline", dock_main);
        ImGui::DockBuilderDockWindow("Source", dock_main);
        ImGui::DockBuilderDockWindow("Details", dock_right);
        ImGui::DockBuilderDockWindow("Filters", dock_right);
        ImGui::DockBuilderDockWindow("Search", dock_bottom);
        ImGui::DockBuilderDockWindow("Statistics", dock_bottom);
        ImGui::DockBuilderDockWindow("Flame Graph", dock_bottom);
        ImGui::DockBuilderDockWindow("Instances", dock_bottom_right);

        ImGui::DockBuilderFinish(dockspace_id);
    } else {
        first_layout_ = false;
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();

    // Menu bar / toolbar
    toolbar_.render(model_, view_, diagnostics_.current_rss_mb());
    if (toolbar_.settings_requested()) {
        show_settings_ = true;
        toolbar_.clear_settings_request();
    }
    if (view_.key_bindings().is_pressed(Action::OpenSettings)) {
        show_settings_ = true;
    }
    if (show_settings_) {
        render_settings_modal();
    }

    if (has_trace_ && !loader_.is_loading()) {
        timeline_.render(model_, view_);
        detail_.render(model_, view_);
        search_.render(model_, view_);
        filter_.render(model_, view_);
        stats_.render(model_, query_db_, view_);
        instances_.render(model_, view_);
        diagnostics_.stats = timeline_.diag_stats;
        source_.render(model_, view_);
        flame_graph_.render(model_, view_);
        diagnostics_.render(model_, view_);
    } else {
        // Welcome screen
        ImGui::Begin("Timeline");
        if (!loader_.is_loading()) {
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

        ImGui::Begin("Flame Graph");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();

        diagnostics_.render(model_, view_);
    }

    // Loading overlay (drawn on top of everything)
    if (loader_.is_loading()) {
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
        if (loader_.is_loading()) {
            std::string ph = loader_.phase();
            if (!ph.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("%s", ph.c_str());
            }
            ImGui::SameLine();
            ImGui::ProgressBar(loader_.phase_progress(), ImVec2(150, 0));
        } else if (query_db_.is_indexing()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("Building query indexes");
            ImGui::SameLine();
            ImGui::ProgressBar(query_db_.indexing_progress(), ImVec2(150, 0));
        }
        if (has_trace_ && !loader_.is_loading() && view_.selected_event_idx() >= 0) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 900);
            const auto& ev = model_.events()[view_.selected_event_idx()];
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
    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Settings", &show_settings_)) {
        const char* tab_labels[] = {"General", "Timeline", "Rendering", "Source", "Keyboard"};
        constexpr int kTabCount = 5;
        constexpr float kSidebarWidth = 140.0f;
        const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

        // --- Sidebar ---
        ImGui::BeginChild("settings_sidebar", ImVec2(kSidebarWidth, -footer_height), ImGuiChildFlags_Borders);
        for (int i = 0; i < kTabCount; i++) {
            if (ImGui::Selectable(tab_labels[i], settings_tab_ == i)) {
                settings_tab_ = i;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // --- Content ---
        ImGui::BeginChild("settings_content", ImVec2(0, -footer_height));

        switch (settings_tab_) {
            case 0: {  // General
                ImGui::SeparatorText("Display");

                float font_scale = ImGui::GetIO().FontGlobalScale;
                if (ImGui::SliderFloat("Font Scale", &font_scale, 0.5f, 6.0f, "%.1f")) {
                    ImGui::GetIO().FontGlobalScale = font_scale;
                }

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

                if (platform::supports_vsync()) {
                    ImGui::SeparatorText("Performance");
                    if (ImGui::Checkbox("VSync", &vsync_)) {
                        SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
                    }
                }

                ImGui::SeparatorText("Parser");

                {
                    bool ns = view_.time_unit_ns();
                    if (ImGui::Checkbox("Interpret timestamps as nanoseconds", &ns)) view_.set_time_unit_ns(ns);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(reload file to apply)");

                ImGui::Spacing();
                ImGui::Spacing();
                if (ImGui::Button("Reset to Defaults")) {
                    reset_general_defaults();
                }
                break;
            }
            case 1: {  // Timeline
                ImGui::SeparatorText("Layout");

                constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
                float v = view_.track_height();
                if (ImGui::DragFloat("Track Height", &v, 1.0f, 20.0f, 200.0f, "%.0f px", clamp))
                    view_.set_track_height(v);
                v = view_.track_padding();
                if (ImGui::DragFloat("Track Padding", &v, 1.0f, 0.0f, 30.0f, "%.0f px", clamp))
                    view_.set_track_padding(v);
                v = view_.counter_track_height();
                if (ImGui::DragFloat("Counter Track Height", &v, 1.0f, 60.0f, 400.0f, "%.0f px", clamp))
                    view_.set_counter_track_height(v);
                v = view_.label_width();
                if (ImGui::DragFloat("Label Gutter Width", &v, 1.0f, 100.0f, 1200.0f, "%.0f px", clamp))
                    view_.set_label_width(v);
                v = view_.ruler_height();
                if (ImGui::DragFloat("Ruler Height", &v, 1.0f, 15.0f, 120.0f, "%.0f px", clamp))
                    view_.set_ruler_height(v);
                v = view_.proc_header_height();
                if (ImGui::DragFloat("Process Header Height", &v, 1.0f, 10.0f, 80.0f, "%.0f px", clamp))
                    view_.set_proc_header_height(v);
                v = view_.scrollbar_scale();
                if (ImGui::DragFloat("Scrollbar Scale", &v, 0.05f, 0.5f, 5.0f, "%.1f", clamp))
                    view_.set_scrollbar_scale(v);

                ImGui::Spacing();
                ImGui::Spacing();
                if (ImGui::Button("Reset to Defaults")) {
                    view_.reset_layout_defaults();
                }
                break;
            }
            case 2: {  // Rendering
                {
                    bool show = view_.show_flows();
                    if (ImGui::Checkbox("Show Flow Arrows", &show)) view_.set_show_flows(show);
                }
                {
                    auto color = view_.sel_border_color();
                    if (ImGui::ColorEdit4("Selection Border Color", color.data(),
                                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
                        view_.set_sel_border_color(color);
                }

                ImGui::Spacing();
                ImGui::Spacing();
                if (ImGui::Button("Reset to Defaults")) {
                    view_.reset_rendering_defaults();
                }
                break;
            }
            case 3: {  // Source
                source_.render_settings();

                ImGui::Spacing();
                ImGui::Spacing();
                if (ImGui::Button("Reset to Defaults")) {
                    source_.reset_settings();
                }
                break;
            }
            case 4: {  // Keyboard
                view_.key_bindings().render_settings();

                ImGui::Spacing();
                ImGui::Spacing();
                if (ImGui::Button("Reset to Defaults")) {
                    view_.key_bindings().reset_defaults();
                }
                break;
            }
        }

        ImGui::EndChild();

        // --- Footer ---
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Reset All", ImVec2(120, 0))) {
            reset_all_defaults();
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);

        if (ImGui::Button("Close", ImVec2(120, 0))) {
            save_settings();
            show_settings_ = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void App::reset_general_defaults() {
    ImGui::GetIO().FontGlobalScale = 1.0f;
    dark_theme_ = true;
    ImGui::StyleColorsDark();
    vsync_ = true;
    SDL_GL_SetSwapInterval(1);
    view_.set_time_unit_ns(false);
}

void App::reset_all_defaults() {
    reset_general_defaults();
    view_.reset_layout_defaults();
    view_.reset_rendering_defaults();
    source_.reset_settings();
    view_.key_bindings().reset_defaults();
}

void App::save_settings() {
    TRACE_FUNCTION_CAT("io");
    std::string path = platform::settings_path();
    if (path.empty()) return;

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    nlohmann::json j;
    j["font_scale"] = ImGui::GetIO().FontGlobalScale;
    j["track_height"] = view_.track_height();
    j["track_padding"] = view_.track_padding();
    j["counter_track_height"] = view_.counter_track_height();
    j["label_width"] = view_.label_width();
    j["show_flows"] = view_.show_flows();
    j["time_unit_ns"] = view_.time_unit_ns();
    j["ruler_height"] = view_.ruler_height();
    j["proc_header_height"] = view_.proc_header_height();
    j["scrollbar_scale"] = view_.scrollbar_scale();
    j["sel_border_color"] = {view_.sel_border_color()[0], view_.sel_border_color()[1], view_.sel_border_color()[2],
                             view_.sel_border_color()[3]};
    j["dark_theme"] = dark_theme_;
    j["vsync"] = vsync_;
    j["query_tabs"] = stats_.save_tabs();
    j["source_panel"] = source_.save_settings();
    j["key_bindings"] = view_.key_bindings().save();

    std::ofstream f(path);
    if (f.is_open()) {
        f << j.dump(2);
    }
}

void App::load_settings() {
    TRACE_FUNCTION_CAT("io");
    std::string path = platform::settings_path();
    if (path.empty()) return;

    std::ifstream f(path);
    if (!f.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(f);

        if (j.contains("font_scale")) ImGui::GetIO().FontGlobalScale = j["font_scale"].get<float>();
        if (j.contains("track_height")) view_.set_track_height(j["track_height"].get<float>());
        if (j.contains("track_padding")) view_.set_track_padding(j["track_padding"].get<float>());
        if (j.contains("counter_track_height")) view_.set_counter_track_height(j["counter_track_height"].get<float>());
        if (j.contains("label_width")) view_.set_label_width(j["label_width"].get<float>());
        if (j.contains("show_flows")) view_.set_show_flows(j["show_flows"].get<bool>());
        if (j.contains("time_unit_ns")) view_.set_time_unit_ns(j["time_unit_ns"].get<bool>());
        if (j.contains("ruler_height")) view_.set_ruler_height(j["ruler_height"].get<float>());
        if (j.contains("proc_header_height")) view_.set_proc_header_height(j["proc_header_height"].get<float>());
        if (j.contains("scrollbar_scale")) view_.set_scrollbar_scale(j["scrollbar_scale"].get<float>());
        if (j.contains("sel_border_color")) {
            auto& arr = j["sel_border_color"];
            std::array<float, 4> col;
            for (int i = 0; i < 4; i++) col[i] = arr[i].get<float>();
            view_.set_sel_border_color(col);
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
        if (j.contains("key_bindings")) {
            view_.key_bindings().load(j["key_bindings"]);
        }
    } catch (...) {
        // Ignore malformed settings file
    }
}
