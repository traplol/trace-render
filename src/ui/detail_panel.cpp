#include "detail_panel.h"
#include "format_time.h"
#include "tracing.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <cctype>
#include <cmath>
#include <unordered_map>

// Returns a color interpolated from blue (cool, 0%) through green/yellow to red (hot, 100%)
static ImVec4 heat_color(float pct) {
    float t = std::min(std::max(pct / 100.0f, 0.0f), 1.0f);
    // Blue -> Cyan -> Green -> Yellow -> Red
    float r, g, b;
    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0.0f;
        g = s;
        b = 1.0f;
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = 0.0f;
        g = 1.0f;
        b = 1.0f - s;
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = s;
        g = 1.0f;
        b = 0.0f;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 1.0f;
        g = 1.0f - s;
        b = 0.0f;
    }
    return ImVec4(r, g, b, 1.0f);
}

static void render_heat_bar(float pct) {
    ImVec4 col = heat_color(pct);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(pct / 100.0f, ImVec2(-1, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetItemRectSize().x);
    ImGui::Text("%.1f%%", pct);
}

static const char* phase_name(Phase ph) {
    switch (ph) {
        case Phase::DurationBegin:
            return "Duration Begin (B)";
        case Phase::DurationEnd:
            return "Duration End (E)";
        case Phase::Complete:
            return "Complete (X)";
        case Phase::Instant:
            return "Instant (i)";
        case Phase::Counter:
            return "Counter (C)";
        case Phase::AsyncBegin:
            return "Async Begin (b)";
        case Phase::AsyncEnd:
            return "Async End (e)";
        case Phase::AsyncInstant:
            return "Async Instant (n)";
        case Phase::FlowStart:
            return "Flow Start (s)";
        case Phase::FlowStep:
            return "Flow Step (t)";
        case Phase::FlowEnd:
            return "Flow End (f)";
        case Phase::Metadata:
            return "Metadata (M)";
        default:
            return "Unknown";
    }
}

static void render_json_value(const nlohmann::json& j, int depth = 0) {
    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_object() || val.is_array()) {
                if (ImGui::TreeNode(key.c_str())) {
                    render_json_value(val, depth + 1);
                    ImGui::TreePop();
                }
            } else {
                ImGui::Text("%s: %s", key.c_str(), val.dump().c_str());
            }
        }
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); i++) {
            char label[32];
            snprintf(label, sizeof(label), "[%zu]", i);
            if (j[i].is_object() || j[i].is_array()) {
                if (ImGui::TreeNode(label)) {
                    render_json_value(j[i], depth + 1);
                    ImGui::TreePop();
                }
            } else {
                ImGui::Text("%s: %s", label, j[i].dump().c_str());
            }
        }
    } else {
        ImGui::Text("%s", j.dump().c_str());
    }
}

void DetailPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Details", "ui");
    ImGui::Begin("Details");

    if (view.selected_event_idx < 0 || view.selected_event_idx >= (int32_t)model.events_.size()) {
        ImGui::TextDisabled("Click a slice in the timeline to see details.");
        ImGui::End();
        return;
    }

    const auto& ev = model.events_[view.selected_event_idx];

    // Eagerly rebuild children cache so self/child time are available for duration display
    if (ev.dur > 0) {
        if (cached_event_idx_ != view.selected_event_idx || cached_descendants_flag_ != include_all_descendants_) {
            cached_event_idx_ = view.selected_event_idx;
            cached_descendants_flag_ = include_all_descendants_;
            rebuild_children(model, ev);
            children_dirty_ = true;
        }
    }

    ImGui::Text("Name: %s", model.get_string(ev.name_idx).c_str());
    ImGui::Separator();

    ImGui::Text("Category: %s", model.get_string(ev.cat_idx).c_str());
    ImGui::Text("Phase: %s", phase_name(ev.ph));

    char time_buf[128];
    format_time((double)ev.ts, time_buf, sizeof(time_buf));
    ImGui::Text("Timestamp: %s", time_buf);

    if (ev.dur > 0) {
        format_time((double)ev.dur, time_buf, sizeof(time_buf));
        ImGui::Text("Wall Time: %s", time_buf);

        double child_time = ev.dur - self_time_;
        float child_pct = (float)(child_time / ev.dur * 100.0);

        // Self time with heat bar
        format_time(self_time_, time_buf, sizeof(time_buf));
        ImGui::Text("Self Time: %s", time_buf);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, heat_color(self_pct_));
        ImGui::ProgressBar(self_pct_ / 100.0f, ImVec2(100, ImGui::GetTextLineHeight()), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f%%", self_pct_);

        // Child time with heat bar
        if (child_time > 0) {
            format_time(child_time, time_buf, sizeof(time_buf));
            ImGui::Text("Child Time: %s", time_buf);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, heat_color(child_pct));
            ImGui::ProgressBar(child_pct / 100.0f, ImVec2(100, ImGui::GetTextLineHeight()), "");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f%%", child_pct);
        }
    }

    ImGui::Text("Process: %u", ev.pid);
    // Find process name
    for (const auto& proc : model.processes_) {
        if (proc.pid == ev.pid) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", proc.name.c_str());
            break;
        }
    }

    ImGui::Text("Thread: %u", ev.tid);
    for (const auto& proc : model.processes_) {
        if (proc.pid == ev.pid) {
            for (const auto& t : proc.threads) {
                if (t.tid == ev.tid) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", t.name.c_str());
                    break;
                }
            }
            break;
        }
    }

    if (ev.id != 0) {
        ImGui::Text("ID: 0x%llx", (unsigned long long)ev.id);
    }

    ImGui::Text("Depth: %d", ev.depth);

    // Parent button
    if (ev.depth > 0) {
        int32_t parent_idx = model.find_parent_event(view.selected_event_idx);
        if (parent_idx >= 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Parent")) {
                view.navigate_to_event(parent_idx, model.events_[parent_idx]);
            }
        }
    }

    ImGui::Separator();

    // --- Tab bar for Call Stack / Children / Arguments ---
    if (ImGui::BeginTabBar("##DetailTabs")) {
        // Call Stack tab
        bool has_stack = (ev.depth > 0);
        char stack_label[32];
        snprintf(stack_label, sizeof(stack_label), "Call Stack (%d)###Stack", ev.depth + 1);
        if (has_stack && ImGui::BeginTabItem(stack_label)) {
            auto stack = model.build_call_stack(view.selected_event_idx);
            for (int i = 0; i < (int)stack.size(); i++) {
                uint32_t idx = stack[i];
                const auto& frame = model.events_[idx];
                bool is_selected = (idx == (uint32_t)view.selected_event_idx);

                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##frame%d", i);
                if (ImGui::Selectable(id_buf, is_selected, ImGuiSelectableFlags_AllowOverlap)) {
                    if (!is_selected) {
                        view.navigate_to_event(idx, frame);
                    }
                }
                ImGui::SameLine();

                // Name with indentation
                char self_buf[64];
                double self = model.compute_self_time(idx);
                format_time(self, self_buf, sizeof(self_buf));

                if (is_selected) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%*s%s", i * 2, "",
                                       model.get_string(frame.name_idx).c_str());
                } else {
                    ImGui::Text("%*s%s", i * 2, "", model.get_string(frame.name_idx).c_str());
                }
                ImGui::SameLine();
                ImGui::TextDisabled("  self: %s", self_buf);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    char wall_buf[64];
                    format_time(frame.dur, wall_buf, sizeof(wall_buf));
                    float self_pct = frame.dur > 0 ? (float)(self / frame.dur * 100.0) : 0.0f;
                    ImGui::SetTooltip("%s\nWall: %s | Self: %s (%.1f%%)", model.get_string(frame.name_idx).c_str(),
                                      wall_buf, self_buf, self_pct);
                }
            }
            ImGui::EndTabItem();
        }

        // Children tab
        {
            // Re-check children cache in case Parent button changed selection
            const auto& current_ev = model.events_[view.selected_event_idx];
            if (current_ev.dur > 0) {
                if (cached_event_idx_ != view.selected_event_idx ||
                    cached_descendants_flag_ != include_all_descendants_) {
                    cached_event_idx_ = view.selected_event_idx;
                    cached_descendants_flag_ = include_all_descendants_;
                    rebuild_children(model, current_ev);
                    children_dirty_ = true;
                }

                if (cached_group_flag_ != group_by_name_ || children_dirty_) {
                    cached_group_flag_ = group_by_name_;
                    if (group_by_name_) {
                        rebuild_aggregated(model, current_ev.dur);
                    }
                    rebuild_filter(model);
                }
            }

            char children_label[32];
            snprintf(children_label, sizeof(children_label), "Children (%zu)###Children", children_.size());
            if (ImGui::BeginTabItem(children_label)) {
                if (children_.empty()) {
                    ImGui::TextDisabled("No children.");
                } else {
                    ImGui::Checkbox("Include all descendants", &include_all_descendants_);
                    ImGui::SameLine();
                    ImGui::Checkbox("Group by name", &group_by_name_);

                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##filter", "Filter by name...", filter_buf_, sizeof(filter_buf_))) {
                        rebuild_filter(model);
                    }
                    if (filter_buf_[0] != '\0') {
                        size_t shown = group_by_name_ ? filtered_aggregated_.size() : filtered_children_.size();
                        size_t total = group_by_name_ ? aggregated_.size() : children_.size();
                        ImGui::TextDisabled("Showing %zu / %zu", shown, total);
                    }

                    ImGui::Spacing();

                    if (group_by_name_) {
                        render_aggregated_table(model, view);
                    } else {
                        render_children_table(model, view);
                    }
                }
                ImGui::EndTabItem();
            }
        }

        // Arguments tab
        {
            bool has_args = (ev.args_idx != UINT32_MAX && ev.args_idx < model.args_.size());
            char args_label[32];
            snprintf(args_label, sizeof(args_label), "Arguments###Args");
            if (ImGui::BeginTabItem(args_label)) {
                if (has_args) {
                    try {
                        auto args = nlohmann::json::parse(model.args_[ev.args_idx]);
                        render_json_value(args);
                    } catch (...) {
                        ImGui::TextDisabled("(could not parse args)");
                        ImGui::TextWrapped("%s", model.args_[ev.args_idx].c_str());
                    }
                } else {
                    ImGui::TextDisabled("No arguments.");
                }
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void DetailPanel::render_aggregated_table(const TraceModel& model, ViewState& view) {
    if (ImGui::BeginTable("AggChildrenTable", 7,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                                0.0f, 2);
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_None, 0.0f, 3);
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_None, 0.0f, 4);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_None, 0.0f, 5);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 6);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (children_dirty_) {
                sort_specs->SpecsDirty = true;
                children_dirty_ = false;
            }
            if (sort_specs->SpecsDirty) {
                sort_specs->SpecsDirty = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(filtered_aggregated_.begin(), filtered_aggregated_.end(), [&](size_t ai, size_t bi) {
                        const auto& a = aggregated_[ai];
                        const auto& b = aggregated_[bi];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0: {
                                const auto& na = model.get_string(a.name_idx);
                                const auto& nb = model.get_string(b.name_idx);
                                cmp = na.compare(nb);
                                break;
                            }
                            case 1:
                                cmp = (a.count < b.count) ? -1 : (a.count > b.count) ? 1 : 0;
                                break;
                            case 2:
                                cmp = (a.total_dur < b.total_dur) ? -1 : (a.total_dur > b.total_dur) ? 1 : 0;
                                break;
                            case 3:
                                cmp = (a.avg_dur < b.avg_dur) ? -1 : (a.avg_dur > b.avg_dur) ? 1 : 0;
                                break;
                            case 4:
                                cmp = (a.min_dur < b.min_dur) ? -1 : (a.min_dur > b.min_dur) ? 1 : 0;
                                break;
                            case 5:
                                cmp = (a.max_dur < b.max_dur) ? -1 : (a.max_dur > b.max_dur) ? 1 : 0;
                                break;
                            case 6:
                                cmp = (a.pct < b.pct) ? -1 : (a.pct > b.pct) ? 1 : 0;
                                break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                }
            }
        }

        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_aggregated_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& ag = aggregated_[filtered_aggregated_[i]];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##ag%d", i);
                if (ImGui::Selectable(id_buf, false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    view.navigate_to_event(ag.longest_idx, model.events_[ag.longest_idx]);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(model.get_string(ag.name_idx).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", model.get_string(ag.name_idx).c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%u", ag.count);

                ImGui::TableNextColumn();
                format_time(ag.total_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.avg_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.min_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.max_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                render_heat_bar(ag.pct);
            }
        }

        ImGui::EndTable();
    }
}

void DetailPanel::render_children_table(const TraceModel& model, ViewState& view) {
    if (ImGui::BeginTable("ChildrenTable", 3,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
        ImGui::TableSetupColumn(
            "Duration", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 1);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 2);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (children_dirty_) {
                sort_specs->SpecsDirty = true;
                children_dirty_ = false;
            }
            if (sort_specs->SpecsDirty) {
                sort_specs->SpecsDirty = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(filtered_children_.begin(), filtered_children_.end(), [&](size_t ai, size_t bi) {
                        const auto& a = children_[ai];
                        const auto& b = children_[bi];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0: {
                                const auto& na = model.get_string(a.name_idx);
                                const auto& nb = model.get_string(b.name_idx);
                                cmp = na.compare(nb);
                                break;
                            }
                            case 1:
                                cmp = (a.dur < b.dur) ? -1 : (a.dur > b.dur) ? 1 : 0;
                                break;
                            case 2:
                                cmp = (a.pct < b.pct) ? -1 : (a.pct > b.pct) ? 1 : 0;
                                break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                }
            }
        }

        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_children_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& c = children_[filtered_children_[i]];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##c%d", i);
                bool is_selected = (view.selected_event_idx == (int32_t)c.event_idx);
                if (ImGui::Selectable(id_buf, is_selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    view.navigate_to_event(c.event_idx, model.events_[c.event_idx]);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(model.get_string(c.name_idx).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", model.get_string(c.name_idx).c_str());

                ImGui::TableNextColumn();
                format_time(c.dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                render_heat_bar(c.pct);
            }
        }

        ImGui::EndTable();
    }
}

void DetailPanel::rebuild_children(const TraceModel& model, const TraceEvent& ev) {
    children_.clear();
    double immediate_children_total = 0.0;

    for (const auto& proc : model.processes_) {
        if (proc.pid != ev.pid) continue;
        for (const auto& thread : proc.threads) {
            if (thread.tid != ev.tid) continue;
            for (uint32_t idx : thread.event_indices) {
                const auto& child = model.events_[idx];
                if (child.depth <= ev.depth) {
                    if (child.ts > ev.end_ts()) break;
                    continue;
                }
                if (child.ts < ev.ts || child.end_ts() > ev.end_ts()) continue;
                if (child.dur <= 0) continue;

                if (child.depth == ev.depth + 1) {
                    immediate_children_total += child.dur;
                }

                if (include_all_descendants_ || child.depth == ev.depth + 1) {
                    float pct = (float)(child.dur / ev.dur * 100.0);
                    children_.push_back({idx, child.name_idx, child.dur, pct});
                }

                if (child.ts > ev.end_ts()) break;
            }
            break;
        }
        break;
    }

    self_time_ = ev.dur - immediate_children_total;
    self_pct_ = (float)(self_time_ / ev.dur * 100.0);
}

void DetailPanel::rebuild_aggregated(const TraceModel& model, double parent_dur) {
    aggregated_.clear();

    // Group by name_idx using a map to accumulator index
    std::unordered_map<uint32_t, size_t> name_to_idx;
    name_to_idx.reserve(children_.size() / 4);

    for (const auto& c : children_) {
        auto it = name_to_idx.find(c.name_idx);
        if (it == name_to_idx.end()) {
            name_to_idx[c.name_idx] = aggregated_.size();
            aggregated_.push_back({c.name_idx, 1, c.dur, 0.0, c.dur, c.dur, 0.0f, c.event_idx});
        } else {
            auto& ag = aggregated_[it->second];
            ag.count++;
            ag.total_dur += c.dur;
            if (c.dur < ag.min_dur) ag.min_dur = c.dur;
            if (c.dur > ag.max_dur) ag.max_dur = c.dur;
            if (c.dur > model.events_[ag.longest_idx].dur) {
                ag.longest_idx = c.event_idx;
            }
        }
    }

    for (auto& ag : aggregated_) {
        ag.avg_dur = ag.total_dur / ag.count;
        ag.pct = (float)(ag.total_dur / parent_dur * 100.0);
    }
}

void DetailPanel::rebuild_filter(const TraceModel& model) {
    active_filter_ = filter_buf_;

    // Convert filter to lowercase for case-insensitive matching
    std::string lower_filter = active_filter_;
    for (auto& ch : lower_filter) ch = (char)std::tolower((unsigned char)ch);

    filtered_children_.clear();
    for (size_t i = 0; i < children_.size(); i++) {
        if (lower_filter.empty()) {
            filtered_children_.push_back(i);
            continue;
        }
        const auto& name = model.get_string(children_[i].name_idx);
        // Case-insensitive substring search
        bool found = false;
        if (name.size() >= lower_filter.size()) {
            for (size_t j = 0; j <= name.size() - lower_filter.size(); j++) {
                bool match = true;
                for (size_t k = 0; k < lower_filter.size(); k++) {
                    if ((char)std::tolower((unsigned char)name[j + k]) != lower_filter[k]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    found = true;
                    break;
                }
            }
        }
        if (found) filtered_children_.push_back(i);
    }

    filtered_aggregated_.clear();
    for (size_t i = 0; i < aggregated_.size(); i++) {
        if (lower_filter.empty()) {
            filtered_aggregated_.push_back(i);
            continue;
        }
        const auto& name = model.get_string(aggregated_[i].name_idx);
        bool found = false;
        if (name.size() >= lower_filter.size()) {
            for (size_t j = 0; j <= name.size() - lower_filter.size(); j++) {
                bool match = true;
                for (size_t k = 0; k < lower_filter.size(); k++) {
                    if ((char)std::tolower((unsigned char)name[j + k]) != lower_filter[k]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    found = true;
                    break;
                }
            }
        }
        if (found) filtered_aggregated_.push_back(i);
    }
}
