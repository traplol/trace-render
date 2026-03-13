#include "toolbar.h"
#include "tracing.h"
#include "imgui.h"
#include <SDL3/SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Global pointer for JS callback
static Toolbar* g_toolbar_instance = nullptr;

extern "C" {
EMSCRIPTEN_KEEPALIVE
void wasm_file_loaded(const char* filename, const char* data, int size) {
    if (g_toolbar_instance) {
        g_toolbar_instance->on_file_data(data, (size_t)size, filename);
    }
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
                    if (!file) return;
                    var reader = new FileReader();
                    reader.onload = function() {
                        var data = new Uint8Array(reader.result);
                        var nameLen = lengthBytesUTF8(file.name) + 1;
                        var namePtr = _malloc(nameLen);
                        stringToUTF8(file.name, namePtr, nameLen);
                        var dataPtr = _malloc(data.length);
                        HEAPU8.set(data, dataPtr);
                        _wasm_file_loaded(namePtr, dataPtr, data.length);
                        _free(namePtr);
                        _free(dataPtr);
                    };
                    reader.readAsArrayBuffer(file);
                    // Reset so the same file can be re-selected
                    input.value = null;
                });
        }
        input.click();
    });
}

void Toolbar::on_file_data(const char* data, size_t size, const char* filename) {
    file_data_.assign(data, data + size);
    file_name_ = filename;
    file_data_ready_ = true;
}

#else  // !__EMSCRIPTEN__

static void file_dialog_callback(void* userdata, const char* const* filelist, int filter) {
    (void)filter;
    auto* toolbar = static_cast<Toolbar*>(userdata);
    if (filelist && filelist[0]) {
        toolbar->on_file_selected(filelist[0]);
    }
}

#endif  // __EMSCRIPTEN__

void Toolbar::on_file_selected(const char* path) {
    file_path_ = path;
    file_open_requested_ = true;
}

void Toolbar::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Toolbar", "ui");
#ifdef __EMSCRIPTEN__
    g_toolbar_instance = this;
#endif
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextDisabled("%.0f FPS", ImGui::GetIO().Framerate);
        ImGui::Separator();
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
#ifdef __EMSCRIPTEN__
                trigger_file_input();
#else
                if (window_) {
                    static const SDL_DialogFileFilter filters[] = {
                        {"JSON Trace Files", "json"},
                        {"All Files", "*"},
                    };
                    SDL_ShowOpenFileDialog(file_dialog_callback, this, window_, filters, 2, nullptr, false);
                } else {
                    show_fallback_dialog_ = true;
                }
#endif
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
#ifdef __EMSCRIPTEN__
        trigger_file_input();
#else
        if (window_) {
            static const SDL_DialogFileFilter filters[] = {
                {"JSON Trace Files", "json"},
                {"All Files", "*"},
            };
            SDL_ShowOpenFileDialog(file_dialog_callback, this, window_, filters, 2, nullptr, false);
        } else {
            show_fallback_dialog_ = true;
        }
#endif
    }

    // Fallback text input dialog (if no window handle available)
    if (show_fallback_dialog_) {
        ImGui::OpenPopup("Open Trace File");
        show_fallback_dialog_ = false;
    }

    if (ImGui::BeginPopupModal("Open Trace File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter trace file path:");
        ImGui::SetNextItemWidth(500);
        bool enter = ImGui::InputText("##path", path_buf_, sizeof(path_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
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
