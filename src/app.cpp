#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>

void App::init(SDL_Window* window) {
    toolbar_.set_window(window);
    load_settings();
}

void App::shutdown() {
    save_settings();
}

void App::open_file(const std::string& path) {
    status_message_ = "Loading: " + path;
    loading_ = true;

    parser_.on_progress = [this](float p) {
        load_progress_ = p;
    };
    parser_.time_unit_ns = view_.time_unit_ns;

    if (parser_.parse(path, model_)) {
        has_trace_ = true;
        // Reset view but preserve layout settings
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
        // Extract filename for status
        std::string filename = path;
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) filename = path.substr(pos + 1);
        query_db_.load(model_);
        status_message_ = "Loaded: " + filename + " (" +
                          std::to_string(model_.events_.size()) + " events, " +
                          std::to_string(model_.processes_.size()) + " processes)";
    } else {
        status_message_ = "Error: " + parser_.error_message;
        has_trace_ = false;
    }

    loading_ = false;
}

void App::update() {
    // Set up dockspace over the entire viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

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
        ImGui::DockBuilderDockWindow("SQL Query", dock_bottom);

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

    if (has_trace_) {
        timeline_.render(model_, view_);
        detail_.render(model_, view_);
        search_.render(model_, view_);
        filter_.render(model_, view_);
        stats_.render(model_, view_);
        query_.render(query_db_, view_);
    } else {
        // Welcome screen
        ImGui::Begin("Timeline");
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 text_size = ImGui::CalcTextSize("Open a Chrome trace file (Ctrl+O) or drag & drop");
        ImGui::SetCursorPos(ImVec2((avail.x - text_size.x) / 2,
                                    (avail.y - text_size.y) / 2));
        ImGui::TextDisabled("Open a Chrome trace file (Ctrl+O) or drag & drop");
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

        ImGui::Begin("SQL Query");
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
    }

    // Status bar
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float status_h = ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - status_h));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, status_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
        ImGui::Begin("##StatusBar", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%s", status_message_.c_str());
        if (loading_) {
            ImGui::SameLine();
            ImGui::ProgressBar(load_progress_, ImVec2(200, 0));
        }
        if (has_trace_ && view_.selected_event_idx >= 0) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 900);
            const auto& ev = model_.events_[view_.selected_event_idx];
            ImGui::Text("Selected: %s", model_.get_string(ev.name_idx).c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void App::render_settings_modal() {
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

        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Show Flow Arrows", &view_.show_flows);

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
    j["dark_theme"] = dark_theme_;

    std::ofstream f(path);
    if (f.is_open()) {
        f << j.dump(2);
    }
}

void App::load_settings() {
    std::string path = settings_path();
    std::ifstream f(path);
    if (!f.is_open()) return;

    try {
        nlohmann::json j = nlohmann::json::parse(f);

        if (j.contains("font_scale"))
            ImGui::GetIO().FontGlobalScale = j["font_scale"].get<float>();
        if (j.contains("track_height"))
            view_.track_height = j["track_height"].get<float>();
        if (j.contains("track_padding"))
            view_.track_padding = j["track_padding"].get<float>();
        if (j.contains("counter_track_height"))
            view_.counter_track_height = j["counter_track_height"].get<float>();
        if (j.contains("label_width"))
            view_.label_width = j["label_width"].get<float>();
        if (j.contains("show_flows"))
            view_.show_flows = j["show_flows"].get<bool>();
        if (j.contains("time_unit_ns"))
            view_.time_unit_ns = j["time_unit_ns"].get<bool>();
        if (j.contains("dark_theme")) {
            dark_theme_ = j["dark_theme"].get<bool>();
            if (dark_theme_)
                ImGui::StyleColorsDark();
            else
                ImGui::StyleColorsLight();
        }
    } catch (...) {
        // Ignore malformed settings file
    }
}
