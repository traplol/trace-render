#include "source_panel.h"
#include "tracing.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cmath>

using json = nlohmann::json;

// Try common field names for source file and line in event args
bool extract_source_location(const TraceModel& model, const TraceEvent& ev, std::string& file, int& line) {
    TRACE_FUNCTION_CAT("io");
    if (ev.args_idx == UINT32_MAX || ev.args_idx >= model.args().size()) return false;

    try {
        auto args = json::parse(model.args()[ev.args_idx]);

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
    TRACE_FUNCTION_CAT("io");
    std::string out = path;
    for (auto& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

std::string remap_source_path(const std::string& trace_path, const std::string& strip_prefix,
                              const std::string& local_base) {
    TRACE_FUNCTION_CAT("io");
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

json SourcePanel::save_settings() const {
    TRACE_FUNCTION_CAT("ui");
    json j;
    j["strip_prefix"] = strip_prefix_;
    j["local_base"] = local_base_;
    return j;
}

void SourcePanel::load_settings(const json& j) {
    TRACE_FUNCTION_CAT("ui");
    if (j.contains("strip_prefix")) {
        snprintf(strip_prefix_, sizeof(strip_prefix_), "%s", j["strip_prefix"].get<std::string>().c_str());
    }
    if (j.contains("local_base")) {
        snprintf(local_base_, sizeof(local_base_), "%s", j["local_base"].get<std::string>().c_str());
    }
}

void SourcePanel::reset_settings() {
    TRACE_FUNCTION_CAT("ui");
    strip_prefix_[0] = '\0';
    local_base_[0] = '\0';
    path_settings_changed_ = true;
}

void SourcePanel::render_settings() {
    TRACE_FUNCTION_CAT("ui");
    ImGui::TextWrapped("Remap source paths from the trace to your local filesystem.");
    ImGui::Spacing();

    float label_w = ImGui::CalcTextSize("Strip prefix ").x;

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Strip prefix");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##strip_setting", "e.g. c:\\jenkins\\prod\\rel-123", strip_prefix_,
                                 sizeof(strip_prefix_))) {
        path_settings_changed_ = true;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Local base");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##base_setting", "e.g. /mnt/c/dev/repo", local_base_, sizeof(local_base_))) {
        path_settings_changed_ = true;
    }
}

void SourcePanel::load_file(const std::string& path) {
    TRACE_FUNCTION_CAT("io");
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
    TRACE_FUNCTION_CAT("io");
    std::string full_path = remap_source_path(raw_file, strip_prefix_, local_base_);

    if (full_path != cached_file_) {
        cached_file_ = full_path;
        load_file(cached_file_);
        build_display_text();
        build_gutter_text();
    }
}

void SourcePanel::build_display_text() {
    TRACE_FUNCTION_CAT("ui");
    cached_display_text_.clear();
    if (cached_lines_.empty()) return;

    size_t estimated = 0;
    for (const auto& line : cached_lines_) estimated += line.size() + 1;
    cached_display_text_.reserve(estimated);

    for (const auto& line : cached_lines_) {
        cached_display_text_ += line;
        cached_display_text_ += '\n';
    }
}

void SourcePanel::build_gutter_text() {
    TRACE_FUNCTION_CAT("ui");
    cached_gutter_text_.clear();
    int num_lines = (int)cached_lines_.size();
    if (num_lines == 0) return;

    int num_digits = (int)std::log10((double)num_lines) + 1;
    if (num_digits < 4) num_digits = 4;
    if (num_digits > 10) num_digits = 10;

    char buf[32];
    for (int i = 1; i <= num_lines; i++) {
        snprintf(buf, sizeof(buf), "%*d", num_digits, i);
        cached_gutter_text_ += buf;
        cached_gutter_text_ += '\n';
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

    if (view.selected_event_idx() < 0 || view.selected_event_idx() >= (int32_t)model.events().size()) {
        ImGui::TextDisabled("Select an event to view source code.");
        ImGui::End();
        return;
    }

    bool selection_changed = (cached_event_idx_ != view.selected_event_idx());
    bool tab_just_shown = ImGui::IsWindowAppearing();

    // Check if selection or path settings changed
    if (selection_changed) {
        cached_event_idx_ = view.selected_event_idx();
        const auto& ev = model.events()[view.selected_event_idx()];

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
            cached_display_text_.clear();
            cached_gutter_text_.clear();
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
    } else if (tab_just_shown && cached_line_ > 0) {
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

    // Source code display: gutter (line numbers) + code (selectable text)
    // Both use InputTextMultiline for identical line spacing — zero drift.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float font_size = ImGui::GetFontSize();

    // Compute gutter width
    int num_lines = (int)cached_lines_.size();
    int num_digits = (num_lines > 0) ? (int)std::log10((double)num_lines) + 1 : 1;
    if (num_digits < 4) num_digits = 4;
    char measure_buf[32];
    snprintf(measure_buf, sizeof(measure_buf), " %*d  ", num_digits, num_lines);
    float gutter_w = ImGui::CalcTextSize(measure_buf).x;
    float code_w = avail.x - gutter_w;

    // Render code first (right side) to obtain scroll state
    ImVec2 region_start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(region_start.x + gutter_w, region_start.y));

    ImGuiID code_child_id = ImGui::GetID("##source_text");
    ImGui::InputTextMultiline("##source_text", &cached_display_text_, ImVec2(code_w, avail.y),
                              ImGuiInputTextFlags_ReadOnly);

    // Find code child window
    ImGuiWindow* code_child = nullptr;
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->ChildId == code_child_id) {
            code_child = w;
            break;
        }

    float scroll_y = 0.0f;
    if (code_child) {
        // Scroll to target line (centered)
        if (need_scroll_ && cached_line_ > 0) {
            code_child->ScrollTarget.y = (cached_line_ - 1) * font_size;
            code_child->ScrollTargetCenterRatio.y = 0.5f;
        }
        scroll_y = code_child->Scroll.y;

        // Draw line highlight
        if (cached_line_ > 0) {
            ImVec2 origin = code_child->ContentRegionRect.Min;
            ImVec2 clip_min = code_child->InnerRect.Min;
            ImVec2 clip_max = code_child->InnerRect.Max;
            float y = origin.y + (cached_line_ - 1) * font_size;
            if (y + font_size > clip_min.y && y < clip_max.y) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(clip_min, clip_max, true);
                dl->AddRectFilled(ImVec2(clip_min.x, ImMax(y, clip_min.y)),
                                  ImVec2(clip_max.x, ImMin(y + font_size, clip_max.y)), IM_COL32(255, 200, 0, 60));
                dl->PopClipRect();
            }
        }
    }

    // Render gutter (left side) — disabled so it's not interactable, no scrollbars
    ImGui::SetCursorPos(region_start);
    ImGui::BeginDisabled();
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    ImGuiID gutter_child_id = ImGui::GetID("##gutter");
    ImGui::InputTextMultiline("##gutter", &cached_gutter_text_, ImVec2(gutter_w, avail.y),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopStyleVar();
    ImGui::EndDisabled();

    // Find gutter child window and sync its scroll to the code area
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->ChildId == gutter_child_id) {
            w->Scroll.y = scroll_y;
            // Also set scroll target on initial navigation so both scroll together
            if (need_scroll_ && cached_line_ > 0) {
                w->ScrollTarget.y = (cached_line_ - 1) * font_size;
                w->ScrollTargetCenterRatio.y = 0.5f;
            }
            break;
        }

    if (need_scroll_) need_scroll_ = false;

    ImGui::End();
}
