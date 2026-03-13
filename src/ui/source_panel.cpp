#include "source_panel.h"
#include "tracing.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>

using json = nlohmann::json;

// Try common field names for source file and line in event args
bool extract_source_location(const TraceModel& model, const TraceEvent& ev, std::string& file, int& line) {
    if (ev.args_idx == UINT32_MAX || ev.args_idx >= model.args_.size()) return false;

    try {
        auto args = json::parse(model.args_[ev.args_idx]);

        // Try common file field names
        static const char* file_keys[] = {"file", "src_file", "fileName", "source_file", "src", "filename"};
        static const char* line_keys[] = {"line", "src_line", "lineNumber", "source_line", "lineno", "line_number"};

        for (const char* key : file_keys) {
            if (args.contains(key) && args[key].is_string()) {
                file = args[key].get<std::string>();
                break;
            }
        }

        if (file.empty()) return false;

        for (const char* key : line_keys) {
            if (args.contains(key)) {
                if (args[key].is_number())
                    line = args[key].get<int>();
                else if (args[key].is_string()) {
                    try {
                        line = std::stoi(args[key].get<std::string>());
                    } catch (...) {}
                }
                break;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

void SourcePanel::load_file(const std::string& path) {
    cached_lines_.clear();
    cached_error_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        cached_error_ = "Could not open: " + path;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        cached_lines_.push_back(std::move(line));
    }
}

void SourcePanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Source", "ui");
    ImGui::Begin("Source");

    // Source root input
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##srcroot", "Source root path (prepended to relative file paths)...", source_root_,
                             sizeof(source_root_));

    ImGui::Separator();

    if (view.selected_event_idx < 0 || view.selected_event_idx >= (int32_t)model.events_.size()) {
        ImGui::TextDisabled("Select an event to view source code.");
        ImGui::End();
        return;
    }

    // Check if selection changed
    if (cached_event_idx_ != view.selected_event_idx) {
        cached_event_idx_ = view.selected_event_idx;
        const auto& ev = model.events_[view.selected_event_idx];

        std::string file;
        int line = -1;

        if (extract_source_location(model, ev, file, line)) {
            // Build full path
            std::string full_path;
            if (!std::filesystem::path(file).is_absolute() && source_root_[0] != '\0') {
                std::string root = source_root_;
                if (!root.empty() && root.back() != '/' && root.back() != '\\') root += '/';
                full_path = root + file;
            } else {
                full_path = file;
            }

            // Only reload if the file changed
            if (full_path != cached_file_) {
                cached_file_ = full_path;
                load_file(cached_file_);
            }
            cached_line_ = line;
            need_scroll_ = true;
        } else {
            cached_file_.clear();
            cached_lines_.clear();
            cached_line_ = -1;
            cached_error_.clear();
        }
    }

    if (cached_file_.empty() && cached_error_.empty()) {
        ImGui::TextDisabled("No source location in this event's args.");
        ImGui::TextDisabled("Expected args fields: file/src_file + line/src_line");
        ImGui::End();
        return;
    }

    // Show file path and line
    ImGui::Text("%s", cached_file_.c_str());
    if (cached_line_ > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled(":%d", cached_line_);
    }

    if (!cached_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", cached_error_.c_str());
        ImGui::End();
        return;
    }

    ImGui::Separator();

    // Source code display
    float line_height = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImGui::BeginChild("##source_scroll", avail, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

    // Scroll to target line
    if (need_scroll_ && cached_line_ > 0) {
        need_scroll_ = false;
        // Center the target line in the visible area
        float target_y = (cached_line_ - 1) * line_height;
        float visible_h = ImGui::GetWindowHeight();
        float scroll_y = target_y - visible_h / 2.0f;
        if (scroll_y < 0) scroll_y = 0;
        ImGui::SetScrollY(scroll_y);
    }

    // Use clipper for large files
    ImGuiListClipper clipper;
    clipper.Begin((int)cached_lines_.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            int line_num = i + 1;
            bool is_target = (line_num == cached_line_);

            if (is_target) {
                // Highlight the target line
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float width = ImGui::GetContentRegionAvail().x + ImGui::GetScrollMaxX();
                ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + line_height),
                                                          IM_COL32(255, 200, 0, 60));
            }

            // Line number (right-aligned in a fixed gutter)
            ImGui::TextDisabled("%5d", line_num);
            ImGui::SameLine();

            if (is_target) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", cached_lines_[i].c_str());
            } else {
                ImGui::TextUnformatted(cached_lines_[i].c_str());
            }
        }
    }

    ImGui::EndChild();
    ImGui::End();
}
