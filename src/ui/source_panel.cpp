#include "source_panel.h"
#include "tracing.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cctype>

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

static std::string normalize_slashes(const std::string& path) {
    std::string out = path;
    for (auto& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

std::string remap_source_path(const std::string& trace_path, const std::string& strip_prefix,
                              const std::string& local_base) {
    std::string path = normalize_slashes(trace_path);
    std::string strip = normalize_slashes(strip_prefix);
    std::string base = normalize_slashes(local_base);

    // Strip trailing slashes from strip prefix
    while (!strip.empty() && strip.back() == '/') strip.pop_back();

    // Case-insensitive prefix match and strip
    if (!strip.empty() && path.size() >= strip.size()) {
        bool match = true;
        for (size_t i = 0; i < strip.size(); i++) {
            if (std::tolower((unsigned char)path[i]) != std::tolower((unsigned char)strip[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            path = path.substr(strip.size());
        }
    }

    // Prepend local base
    if (!base.empty()) {
        // Strip trailing slash from base, ensure single separator between base and path
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (!path.empty() && path.front() != '/') {
            path = base + "/" + path;
        } else {
            path = base + path;
        }
    }

    return path;
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

void SourcePanel::resolve_and_load(const std::string& raw_file) {
    std::string full_path = remap_source_path(raw_file, strip_prefix_, local_base_);

    if (full_path != cached_file_) {
        cached_file_ = full_path;
        load_file(cached_file_);
    }
}

void SourcePanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Source", "ui");
    ImGui::Begin("Source");

    // Path remapping inputs
    float label_w = ImGui::CalcTextSize("Local base ").x;

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Strip prefix");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##strip", "e.g. c:\\jenkins\\prod\\rel-123", strip_prefix_, sizeof(strip_prefix_))) {
        path_settings_changed_ = true;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Local base");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##base", "e.g. /mnt/c/dev/repo", local_base_, sizeof(local_base_))) {
        path_settings_changed_ = true;
    }

    ImGui::Separator();

    if (view.selected_event_idx < 0 || view.selected_event_idx >= (int32_t)model.events_.size()) {
        ImGui::TextDisabled("Select an event to view source code.");
        ImGui::End();
        return;
    }

    bool selection_changed = (cached_event_idx_ != view.selected_event_idx);

    // Check if selection or path settings changed
    if (selection_changed) {
        cached_event_idx_ = view.selected_event_idx;
        const auto& ev = model.events_[view.selected_event_idx];

        std::string file;
        int line = -1;

        if (extract_source_location(model, ev, file, line)) {
            cached_raw_file_ = file;
            resolve_and_load(cached_raw_file_);
            cached_line_ = line;
            need_scroll_ = true;
        } else {
            cached_raw_file_.clear();
            cached_file_.clear();
            cached_lines_.clear();
            cached_line_ = -1;
            cached_error_.clear();
        }
        path_settings_changed_ = false;
    } else if (path_settings_changed_ && !cached_raw_file_.empty()) {
        // Path settings changed — re-resolve without changing selection
        path_settings_changed_ = false;
        cached_file_.clear();  // force reload
        resolve_and_load(cached_raw_file_);
        need_scroll_ = true;
    }

    if (cached_raw_file_.empty()) {
        ImGui::TextDisabled("No source location in this event's args.");
        ImGui::TextDisabled("Expected args fields: file/src_file + line/src_line");
        ImGui::End();
        return;
    }

    // Show raw and resolved paths
    ImGui::TextDisabled("Trace: %s", cached_raw_file_.c_str());
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
