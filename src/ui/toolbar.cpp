#include "toolbar.h"
#include "imgui.h"
#include <SDL3/SDL.h>

static void file_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    (void)filter;
    auto* toolbar = static_cast<Toolbar*>(userdata);
    if (filelist && filelist[0]) {
        toolbar->on_file_selected(filelist[0]);
    }
}

void Toolbar::on_file_selected(const char* path) {
    file_path_ = path;
    file_open_requested_ = true;
}

void Toolbar::render(const TraceModel& model, ViewState& view) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                if (window_) {
                    static const SDL_DialogFileFilter filters[] = {
                        {"JSON Trace Files", "json"},
                        {"All Files", "*"},
                    };
                    SDL_ShowOpenFileDialog(file_dialog_callback, this, window_,
                                         filters, 2, nullptr, false);
                } else {
                    show_fallback_dialog_ = true;
                }
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
        if (window_) {
            static const SDL_DialogFileFilter filters[] = {
                {"JSON Trace Files", "json"},
                {"All Files", "*"},
            };
            SDL_ShowOpenFileDialog(file_dialog_callback, this, window_,
                                 filters, 2, nullptr, false);
        } else {
            show_fallback_dialog_ = true;
        }
    }

    // Fallback text input dialog (if no window handle available)
    if (show_fallback_dialog_) {
        ImGui::OpenPopup("Open Trace File");
        show_fallback_dialog_ = false;
    }

    if (ImGui::BeginPopupModal("Open Trace File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter trace file path:");
        ImGui::SetNextItemWidth(500);
        bool enter = ImGui::InputText("##path", path_buf_, sizeof(path_buf_),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Open")) {
            file_path_ = path_buf_;
            file_open_requested_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
