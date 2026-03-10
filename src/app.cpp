#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <filesystem>

void App::init(SDL_Window* window) {
    toolbar_.set_window(window);
}

void App::open_file(const std::string& path) {
    status_message_ = "Loading: " + path;
    loading_ = true;

    parser_.on_progress = [this](float p) {
        load_progress_ = p;
    };

    if (parser_.parse(path, model_)) {
        has_trace_ = true;
        view_ = ViewState{}; // Reset view
        if (model_.min_ts_ < model_.max_ts_) {
            view_.zoom_to_fit(model_.min_ts_, model_.max_ts_);
        }
        // Extract filename for status
        std::string filename = path;
        auto pos = path.find_last_of("/\\");
        if (pos != std::string::npos) filename = path.substr(pos + 1);
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

    if (has_trace_) {
        timeline_.render(model_, view_);
        detail_.render(model_, view_);
        search_.render(model_, view_);
        filter_.render(model_, view_);
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
