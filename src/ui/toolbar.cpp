#include "toolbar.h"
#include "platform/platform.h"
#include "tracing.h"
#include "imgui.h"
#include <SDL3/SDL.h>

void Toolbar::render(const TraceModel& model, ViewState& view, float rss_mb) {
    TRACE_SCOPE_CAT("Toolbar", "ui");
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextDisabled("%.0f FPS", ImGui::GetIO().Framerate);
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f MB", rss_mb);
        ImGui::Separator();
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                platform::open_file_dialog(window_);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom to Fit", "F")) {
                if (model.min_ts_ < model.max_ts_) {
                    view.zoom_to_fit(model.min_ts_, model.max_ts_);
                }
            }
            ImGui::MenuItem("Show Flow Arrows", nullptr, &view.show_flows);
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
                settings_requested_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Ctrl+O shortcut
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        platform::open_file_dialog(window_);
    }
}
