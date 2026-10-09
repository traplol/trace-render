#include "memory_panel.h"
#include "format_time.h"
#include "imgui.h"
#include <algorithm>

namespace {
std::string frame_name(const TraceModel& model, int32_t frame) {
    if (frame < 0) return "Unknown allocation stack";
    const auto& item = model.stack_frames()[frame];
    const auto& name = model.get_string(item.name_idx);
    if (!name.empty()) return name;
    if (item.address) {
        char text[32];
        snprintf(text, sizeof(text), "0x%llx", (unsigned long long)*item.address);
        return text;
    }
    return "Unresolved frame";
}

std::vector<int32_t> stack_path(const TraceModel& model, int32_t leaf) {
    std::vector<int32_t> result;
    for (int32_t i = leaf; i >= 0; i = model.stack_frames()[i].parent_idx) result.push_back(i);
    std::reverse(result.begin(), result.end());
    return result;
}

bool greater_amount(const OutstandingMemoryAmount& a, const OutstandingMemoryAmount& b) {
    if (a.known.bytes != b.known.bytes) return a.known.bytes > b.known.bytes;
    if (a.uncertain.bytes != b.uncertain.bytes) return a.uncertain.bytes > b.uncertain.bytes;
    return a.known.count > b.known.count;
}
}  // namespace

void MemoryPanel::on_model_changed() {
    initialized_ = false;
    dirty_ = true;
    time_ = 0;
    process_id_.clear();
    birth_range_.reset();
    result_ = {};
    stack_labels_.clear();
    selected_function_ = selected_stack_ = -1;
    contributing_stacks_.clear();
    selected_path_.clear();
}

void MemoryPanel::set_time(double ts) {
    initialized_ = true;
    if (time_ == ts) return;
    time_ = ts;
    dirty_ = true;
}

void MemoryPanel::set_process(std::string process_id) {
    if (process_id_ == process_id) return;
    process_id_ = std::move(process_id);
    dirty_ = true;
}

void MemoryPanel::set_birth_range(std::optional<std::pair<double, double>> range) {
    if (birth_range_ == range) return;
    birth_range_ = range;
    dirty_ = true;
}

void MemoryPanel::refresh(const TraceModel& model) {
    if (!initialized_) {
        time_ = model.profile().capture_end_ts.value_or(model.min_ts() <= model.max_ts() ? model.max_ts() : 0);
        initialized_ = true;
    }
    if (!dirty_) return;
    std::optional<std::pair<std::string, int32_t>> function, stack;
    if (selected_function_ >= 0) {
        const auto& selected = result_.functions[selected_function_];
        function = {selected.process_id, selected.frame_index};
    }
    if (selected_stack_ >= 0) {
        const auto& selected = result_.stacks[selected_stack_];
        stack = {selected.process_id, selected.frame_index};
    }
    result_ = model.query_outstanding_memory(time_, birth_range_, process_id_);
    std::stable_sort(result_.functions.begin(), result_.functions.end(),
                     [](const auto& a, const auto& b) { return greater_amount(a.inclusive, b.inclusive); });
    std::stable_sort(result_.stacks.begin(), result_.stacks.end(),
                     [](const auto& a, const auto& b) { return greater_amount(a.total, b.total); });
    stack_labels_.clear();
    for (const auto& row : result_.stacks) {
        std::string label;
        for (int32_t frame : stack_path(model, row.frame_index)) {
            if (!label.empty()) label += " > ";
            label += frame_name(model, frame);
        }
        if (label.empty()) label = "Unknown allocation stack";
        stack_labels_.push_back(std::move(label));
    }
    selected_function_ = selected_stack_ = -1;
    contributing_stacks_.clear();
    selected_path_.clear();
    if (function) {
        for (size_t i = 0; i < result_.functions.size(); ++i) {
            const auto& row = result_.functions[i];
            if (row.process_id == function->first && row.frame_index == function->second) {
                select_function(model, (int32_t)i);
                break;
            }
        }
    }
    if (stack) {
        for (size_t i : contributing_stacks_) {
            const auto& row = result_.stacks[i];
            if (row.process_id == stack->first && row.frame_index == stack->second) {
                select_stack(model, (int32_t)i);
                break;
            }
        }
    }
    dirty_ = false;
}

void MemoryPanel::select_function(const TraceModel& model, int32_t row) {
    selected_function_ = row >= 0 && (size_t)row < result_.functions.size() ? row : -1;
    selected_stack_ = -1;
    contributing_stacks_.clear();
    selected_path_.clear();
    if (selected_function_ < 0) return;
    const auto& function = result_.functions[selected_function_];
    for (size_t i = 0; i < result_.stacks.size(); ++i) {
        const auto& stack = result_.stacks[i];
        if (stack.process_id == function.process_id &&
            model.memory_stack_contains_function(stack.frame_index, function.frame_index))
            contributing_stacks_.push_back(i);
    }
}

void MemoryPanel::select_stack(const TraceModel& model, int32_t row) {
    selected_stack_ = row >= 0 && std::find(contributing_stacks_.begin(), contributing_stacks_.end(), (size_t)row) !=
                                      contributing_stacks_.end()
                          ? row
                          : -1;
    selected_path_.clear();
    if (selected_stack_ >= 0) selected_path_ = stack_path(model, result_.stacks[selected_stack_].frame_index);
}

void MemoryPanel::render(const TraceModel& model, ViewState& view) {
    if (!ImGui::Begin("Memory")) {
        ImGui::End();
        return;
    }
    refresh(model);
    if (!model.profile().capabilities.native_allocation_history) {
        ImGui::TextWrapped(
            "Native allocation history is unavailable. Outstanding native memory cannot be attributed from this "
            "profile.");
        if (model.profile().capabilities.managed_heap_snapshots)
            ImGui::TextWrapped("Heap snapshots alone do not record allocation origins.");
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Native outstanding memory");
    double edited_time = time_;
    ImGui::SetNextItemWidth(190);
    if (ImGui::InputDouble("T (microseconds)", &edited_time, 0, 0, "%.6f")) set_time(edited_time);
    if (model.profile().capture_end_ts) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Capture end")) set_time(*model.profile().capture_end_ts);
    }
    if (view.selected_event_idx() >= 0 && (size_t)view.selected_event_idx() < model.events().size()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Selected event time")) set_time(model.events()[view.selected_event_idx()].ts);
    }
    if (ImGui::BeginCombo("Process lifetime", process_id_.empty() ? "All processes" : process_id_.c_str())) {
        if (ImGui::Selectable("All processes", process_id_.empty())) set_process({});
        for (const auto& process : model.profile().processes) {
            const std::string label = "PID " + std::to_string(process.pid) + " | " + process.id;
            if (ImGui::Selectable(label.c_str(), process_id_ == process.id)) set_process(process.id);
        }
        ImGui::EndCombo();
    }
    bool filter_births = birth_range_.has_value();
    if (ImGui::Checkbox("Filter allocation birth time", &filter_births))
        set_birth_range(
            filter_births
                ? std::optional<std::pair<double, double>>{{model.profile().capture_start_ts.value_or(model.min_ts()),
                                                            time_}}
                : std::nullopt);
    if (view.has_range_selection()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use timeline range")) set_birth_range({{view.range_start_ts(), view.range_end_ts()}});
    }
    if (birth_range_) {
        double start = birth_range_->first, end = birth_range_->second;
        ImGui::SetNextItemWidth(190);
        bool changed = ImGui::InputDouble("Born at or after (us)", &start, 0, 0, "%.6f");
        ImGui::SetNextItemWidth(190);
        changed |= ImGui::InputDouble("Born before (us)", &end, 0, 0, "%.6f");
        if (changed) set_birth_range({{start, end}});
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear birth filter")) set_birth_range(std::nullopt);
    }
    refresh(model);
    if (!result_.error.empty()) {
        ImGui::TextWrapped("%s", result_.error.c_str());
        ImGui::End();
        return;
    }
    char time_text[64];
    format_time(time_, time_text, sizeof(time_text));
    ImGui::Text("Known outstanding at %s: %llu bytes in %llu allocations", time_text,
                (unsigned long long)result_.total.known.bytes, (unsigned long long)result_.total.known.count);
    if (result_.total.uncertain.count)
        ImGui::TextWrapped("Uncertain records: %llu bytes in %llu allocations. Their presence at T is unproven.",
                           (unsigned long long)result_.total.uncertain.bytes,
                           (unsigned long long)result_.total.uncertain.count);
    if (result_.incomplete)
        ImGui::TextWrapped(
            "Partial capture. These totals cover recorded allocations; unobserved memory may be missing.");
    if (model.profile().quality.sampled_allocations)
        ImGui::TextWrapped("Allocation events were sampled. Values describe recorded samples, not the complete heap.");
    if (result_.unknown_birth_count)
        ImGui::TextWrapped("Birth filter omitted %llu allocations whose start time is unknown.",
                           (unsigned long long)result_.unknown_birth_count);
    for (const auto& warning : model.profile().quality.warnings) ImGui::TextWrapped("%s", warning.c_str());
    ImGui::TextDisabled("Outstanding allocation bytes; not process RAM, total allocated bytes, or CPU time.");
    ImGui::Separator();
    ImGui::TextUnformatted("Functions on allocation stacks");
    ImGui::TextDisabled("Inclusive rows overlap. The summary counts each allocation once.");
    if (result_.functions.empty()) ImGui::TextDisabled("No recorded allocations match this time and birth filter.");
    if (ImGui::BeginTable("MemoryFunctions", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 230))) {
        for (const char* label : {"Function", "Process lifetime", "Known incl bytes", "Known count",
                                  "Uncertain incl bytes", "Uncertain count", "Known leaf bytes"})
            ImGui::TableSetupColumn(label);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)result_.functions.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& row = result_.functions[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(frame_name(model, row.frame_index).c_str(), selected_function_ == i))
                    select_function(model, i);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.process_id.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)row.inclusive.known.bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)row.inclusive.known.count);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)row.inclusive.uncertain.bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)row.inclusive.uncertain.count);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", (unsigned long long)row.exclusive.known.bytes);
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (selected_function_ < 0) {
        ImGui::TextDisabled("Select a function to inspect its contributing allocation paths.");
    } else {
        ImGui::Text("Allocation paths through %s",
                    frame_name(model, result_.functions[selected_function_].frame_index).c_str());
        if (ImGui::BeginTable("MemoryStacks", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 150))) {
            for (const char* label :
                 {"Allocation call path", "Known bytes", "Known count", "Uncertain bytes", "Uncertain count"})
                ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin((int)contributing_stacks_.size());
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    size_t index = contributing_stacks_[i];
                    const auto& row = result_.stacks[index];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(stack_labels_[index].c_str(), selected_stack_ == (int32_t)index))
                        select_stack(model, (int32_t)index);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", stack_labels_[index].c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)row.total.known.bytes);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)row.total.known.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)row.total.uncertain.bytes);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)row.total.uncertain.count);
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        if (selected_stack_ >= 0 && selected_path_.empty())
            ImGui::TextDisabled("The allocation origin stack was not recorded.");
        for (int32_t index : selected_path_) {
            const auto& frame = model.stack_frames()[index];
            ImGui::PushID(index);
            ImGui::TextUnformatted(frame_name(model, index).c_str());
            if (frame.source_file != 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("View source")) {
                    view.select_stack_frame(index);
                    ImGui::SetWindowFocus("Source");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s:%u", model.get_string(frame.source_file).c_str(), frame.source_line);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("Source unavailable");
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}
