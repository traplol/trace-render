#include "instance_panel.h"
#include "format_time.h"
#include "sort_utils.h"
#include "tracing.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

void InstancePanel::select_function_by_name(const std::string& name, const TraceModel& model) {
    TRACE_FUNCTION_CAT("ui");
    selected_name_ = name;
    instances_.clear();
    instance_cursor_ = -1;

    auto it = model.string_map().find(name);
    if (it == model.string_map().end()) return;

    auto ev_it = model.name_to_events().find(it->second);
    if (ev_it == model.name_to_events().end()) return;

    instances_ = ev_it->second;  // already sorted by timestamp
}

void InstancePanel::navigate_to_instance(int32_t idx, const TraceModel& model, ViewState& view) {
    if (idx < 0 || idx >= (int32_t)instances_.size()) return;
    instance_cursor_ = idx;
    uint32_t ev_idx = instances_[idx];
    view.navigate_to_event(ev_idx, model.events()[ev_idx]);
}

void InstancePanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Instances", "ui");
    ImGui::Begin("Instances");

    if (model.events().empty()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    // Track external selection changes
    {
        TRACE_SCOPE_CAT("TrackSelection", "ui");
        if (view.selected_event_idx() != last_selected_event_ && view.selected_event_idx() >= 0) {
            last_selected_event_ = view.selected_event_idx();
            const auto& ev = model.events()[view.selected_event_idx()];
            if (!ev.is_end_event && ev.ph != Phase::Metadata && ev.ph != Phase::Counter) {
                const std::string& name = model.get_string(ev.name_idx);
                if (selected_name_ != name) {
                    select_function_by_name(name, model);
                    instances_dirty_ = true;
                }
                scroll_to_cursor_ = true;
            }
        } else if (view.selected_event_idx() < 0 && last_selected_event_ >= 0) {
            last_selected_event_ = -1;
        }
    }

    if (selected_name_.empty() || instances_.empty()) {
        ImGui::TextDisabled("Select an event to browse instances.");
        ImGui::End();
        return;
    }

    ImGui::Text("Instances of \"%s\"", selected_name_.c_str());
    ImGui::SameLine();
    ImGui::Text("(%d / %zu)", instance_cursor_ + 1, instances_.size());

    ImGui::SameLine();
    if (ImGui::Button("<##prev") && instance_cursor_ > 0) {
        navigate_to_instance(instance_cursor_ - 1, model, view);
    }
    ImGui::SameLine();
    if (ImGui::Button(">##next") && instance_cursor_ < (int32_t)instances_.size() - 1) {
        navigate_to_instance(instance_cursor_ + 1, model, view);
    }

    if (ImGui::BeginTable("InstancesTable", 2,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableHeadersRow();

        {
            TRACE_SCOPE_CAT("SortInstances", "ui");
            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (instances_dirty_) {
                    sort_specs->SpecsDirty = true;
                }
                if (sort_specs->SpecsDirty) {
                    bool user_sorted = !instances_dirty_;
                    sort_specs->SpecsDirty = false;
                    instances_dirty_ = false;
                    if (sort_specs->SpecsCount > 0) {
                        const auto& spec = sort_specs->Specs[0];
                        bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                        std::sort(instances_.begin(), instances_.end(), [&](uint32_t a_idx, uint32_t b_idx) {
                            const auto& a = model.events()[a_idx];
                            const auto& b = model.events()[b_idx];
                            int cmp = 0;
                            switch (spec.ColumnUserID) {
                                case 0:
                                    cmp = sort_utils::three_way_cmp(a.ts, b.ts);
                                    break;
                                case 1:
                                    cmp = sort_utils::three_way_cmp(a.dur, b.dur);
                                    break;
                            }
                            return asc ? (cmp < 0) : (cmp > 0);
                        });
                    }
                    // Re-find cursor after sort
                    if (view.selected_event_idx() >= 0) {
                        instance_cursor_ = -1;
                        for (int i = 0; i < (int)instances_.size(); i++) {
                            if (instances_[i] == (uint32_t)view.selected_event_idx()) {
                                instance_cursor_ = i;
                                break;
                            }
                        }
                    }
                    if (user_sorted) {
                        scroll_to_top_ = true;
                    }
                }
            }
        }

        if (scroll_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top_ = false;
        }

        TRACE_SCOPE_CAT("DrawRows", "ui");
        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)instances_.size());
        if (scroll_to_cursor_ && instance_cursor_ >= 0) {
            clipper.IncludeItemByIndex(instance_cursor_);
        }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                uint32_t ev_idx = instances_[i];
                const auto& ev = model.events()[ev_idx];

                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                format_time(ev.ts, buf, sizeof(buf));
                bool is_selected = (instance_cursor_ == i);
                char id_buf[80];
                snprintf(id_buf, sizeof(id_buf), "%s##i%d", buf, i);
                if (ImGui::Selectable(id_buf, is_selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    navigate_to_instance(i, model, view);
                }
                if (scroll_to_cursor_ && i == instance_cursor_) {
                    ImGui::SetScrollHereY(0.5f);
                    scroll_to_cursor_ = false;
                }

                ImGui::TableNextColumn();
                format_time(ev.dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
