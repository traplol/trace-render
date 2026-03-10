#include "stats_panel.h"
#include "imgui.h"
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cfloat>
#include <cmath>

static void format_time_stats(double us, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);
    if (abs_us < 1.0)
        snprintf(buf, buf_size, "%.1f ns", us * 1000.0);
    else if (abs_us < 1000.0)
        snprintf(buf, buf_size, "%.3f us", us);
    else if (abs_us < 1000000.0)
        snprintf(buf, buf_size, "%.3f ms", us / 1000.0);
    else
        snprintf(buf, buf_size, "%.3f s", us / 1000000.0);
}

void StatsPanel::rebuild(const TraceModel& model, const ViewState& view) {
    stats_.clear();
    selected_func_ = -1;
    instances_.clear();
    instance_cursor_ = -1;
    std::unordered_map<uint32_t, size_t> name_to_idx;

    for (const auto& ev : model.events_) {
        if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
            continue;
        if (ev.dur <= 0) continue;

        if (view.hidden_pids.count(ev.pid)) continue;
        if (view.hidden_tids.count(ev.tid)) continue;
        if (view.hidden_cats.count(ev.cat_idx)) continue;

        auto it = name_to_idx.find(ev.name_idx);
        if (it == name_to_idx.end()) {
            name_to_idx[ev.name_idx] = stats_.size();
            FuncStats fs;
            fs.name_idx = ev.name_idx;
            fs.count = 1;
            fs.total_dur = ev.dur;
            fs.min_dur = ev.dur;
            fs.max_dur = ev.dur;
            stats_.push_back(fs);
        } else {
            auto& fs = stats_[it->second];
            fs.count++;
            fs.total_dur += ev.dur;
            if (ev.dur < fs.min_dur) fs.min_dur = ev.dur;
            if (ev.dur > fs.max_dur) fs.max_dur = ev.dur;
        }
    }

    for (auto& fs : stats_) {
        fs.avg_dur = fs.total_dur / fs.count;
    }

    last_event_count_ = (uint32_t)model.events_.size();
    needs_rebuild_ = false;
}

void StatsPanel::select_function(int32_t func_idx, const TraceModel& model, const ViewState& view) {
    selected_func_ = func_idx;
    instances_.clear();
    instance_cursor_ = -1;

    if (func_idx < 0 || func_idx >= (int32_t)stats_.size()) return;

    uint32_t name_idx = stats_[func_idx].name_idx;
    for (uint32_t i = 0; i < (uint32_t)model.events_.size(); i++) {
        const auto& ev = model.events_[i];
        if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
            continue;
        if (ev.dur <= 0) continue;
        if (ev.name_idx != name_idx) continue;
        if (view.hidden_pids.count(ev.pid)) continue;
        if (view.hidden_tids.count(ev.tid)) continue;
        if (view.hidden_cats.count(ev.cat_idx)) continue;
        instances_.push_back(i);
    }

    // Sort instances by timestamp
    std::sort(instances_.begin(), instances_.end(),
        [&](uint32_t a, uint32_t b) {
            return model.events_[a].ts < model.events_[b].ts;
        });
}

void StatsPanel::navigate_to_instance(int32_t idx, const TraceModel& model, ViewState& view) {
    if (idx < 0 || idx >= (int32_t)instances_.size()) return;
    instance_cursor_ = idx;
    uint32_t ev_idx = instances_[idx];
    view.selected_event_idx = ev_idx;
    const auto& ev = model.events_[ev_idx];
    double pad = std::max(ev.dur * 0.5, 100.0);
    view.view_start_ts = ev.ts - pad;
    view.view_end_ts = ev.end_ts() + pad;
}

void StatsPanel::render(const TraceModel& model, ViewState& view) {
    ImGui::Begin("Statistics");

    if (model.events_.empty()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    if (needs_rebuild_ || last_event_count_ != (uint32_t)model.events_.size()) {
        rebuild(model, view);
    }

    if (ImGui::Button("Refresh")) {
        rebuild(model, view);
    }
    ImGui::SameLine();
    ImGui::Text("%zu functions", stats_.size());

    if (stats_.empty()) {
        ImGui::TextDisabled("No duration events found.");
        ImGui::End();
        return;
    }

    // Use a splitter: top half for summary table, bottom half for instance browser
    float avail_h = ImGui::GetContentRegionAvail().y;
    float table_h = (selected_func_ >= 0) ? avail_h * 0.5f : 0.0f;

    if (ImGui::BeginTable("StatsTable", 6,
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable,
            ImVec2(0, table_h > 0 ? avail_h - table_h : 0))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_None,              0.0f, 0);
        ImGui::TableSetupColumn("Count",    ImGuiTableColumnFlags_None,              0.0f, 1);
        ImGui::TableSetupColumn("Total",    ImGuiTableColumnFlags_DefaultSort |
                                            ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 2);
        ImGui::TableSetupColumn("Avg",      ImGuiTableColumnFlags_None,              0.0f, 3);
        ImGui::TableSetupColumn("Min",      ImGuiTableColumnFlags_None,              0.0f, 4);
        ImGui::TableSetupColumn("Max",      ImGuiTableColumnFlags_None,              0.0f, 5);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty) {
                sort_specs->SpecsDirty = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(stats_.begin(), stats_.end(),
                        [&](const FuncStats& a, const FuncStats& b) {
                            int cmp = 0;
                            switch (spec.ColumnUserID) {
                                case 0: {
                                    const auto& na = model.get_string(a.name_idx);
                                    const auto& nb = model.get_string(b.name_idx);
                                    cmp = na.compare(nb);
                                    break;
                                }
                                case 1: cmp = (a.count < b.count) ? -1 : (a.count > b.count) ? 1 : 0; break;
                                case 2: cmp = (a.total_dur < b.total_dur) ? -1 : (a.total_dur > b.total_dur) ? 1 : 0; break;
                                case 3: cmp = (a.avg_dur < b.avg_dur) ? -1 : (a.avg_dur > b.avg_dur) ? 1 : 0; break;
                                case 4: cmp = (a.min_dur < b.min_dur) ? -1 : (a.min_dur > b.min_dur) ? 1 : 0; break;
                                case 5: cmp = (a.max_dur < b.max_dur) ? -1 : (a.max_dur > b.max_dur) ? 1 : 0; break;
                            }
                            return asc ? (cmp < 0) : (cmp > 0);
                        });
                    // Invalidate selected_func_ since sort changed indices
                    selected_func_ = -1;
                    instances_.clear();
                    instance_cursor_ = -1;
                }
            }
        }

        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)stats_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& fs = stats_[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##s%d", i);
                bool is_selected = (selected_func_ == i);
                if (ImGui::Selectable(id_buf, is_selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    select_function(i, model, view);
                    if (!instances_.empty()) {
                        navigate_to_instance(0, model, view);
                    }
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(model.get_string(fs.name_idx).c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%u", fs.count);

                ImGui::TableNextColumn();
                format_time_stats(fs.total_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time_stats(fs.avg_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time_stats(fs.min_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time_stats(fs.max_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);
            }
        }

        ImGui::EndTable();
    }

    // Instance browser
    if (selected_func_ >= 0 && selected_func_ < (int32_t)stats_.size()) {
        ImGui::Separator();

        const auto& fs = stats_[selected_func_];
        ImGui::Text("Instances of \"%s\"", model.get_string(fs.name_idx).c_str());
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
        ImGui::SameLine();
        if (ImGui::Button("Close##instances")) {
            selected_func_ = -1;
            instances_.clear();
            instance_cursor_ = -1;
        }

        if (ImGui::BeginTable("InstancesTable", 4,
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable,
                ImVec2(0, 0))) {

            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("##Num",    ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 80.0f, 0);
            ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.0f, 2);
            ImGui::TableSetupColumn("Thread",   ImGuiTableColumnFlags_None, 0.0f, 3);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                if (sort_specs->SpecsDirty) {
                    sort_specs->SpecsDirty = false;
                    if (sort_specs->SpecsCount > 0) {
                        const auto& spec = sort_specs->Specs[0];
                        bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                        std::sort(instances_.begin(), instances_.end(),
                            [&](uint32_t a_idx, uint32_t b_idx) {
                                const auto& a = model.events_[a_idx];
                                const auto& b = model.events_[b_idx];
                                int cmp = 0;
                                switch (spec.ColumnUserID) {
                                    case 1: cmp = (a.ts < b.ts) ? -1 : (a.ts > b.ts) ? 1 : 0; break;
                                    case 2: cmp = (a.dur < b.dur) ? -1 : (a.dur > b.dur) ? 1 : 0; break;
                                    case 3: cmp = (a.tid < b.tid) ? -1 : (a.tid > b.tid) ? 1 : 0; break;
                                }
                                return asc ? (cmp < 0) : (cmp > 0);
                            });
                    }
                }
            }

            char buf[64];
            ImGuiListClipper clipper;
            clipper.Begin((int)instances_.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    uint32_t ev_idx = instances_[i];
                    const auto& ev = model.events_[ev_idx];

                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "%d##i%d", i + 1, i);
                    bool is_selected = (instance_cursor_ == i);
                    if (ImGui::Selectable(id_buf, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        navigate_to_instance(i, model, view);
                    }

                    ImGui::TableNextColumn();
                    format_time_stats(ev.ts, buf, sizeof(buf));
                    ImGui::TextUnformatted(buf);

                    ImGui::TableNextColumn();
                    format_time_stats(ev.dur, buf, sizeof(buf));
                    ImGui::TextUnformatted(buf);

                    ImGui::TableNextColumn();
                    // Find thread name
                    bool found_name = false;
                    for (const auto& proc : model.processes_) {
                        if (proc.pid != ev.pid) continue;
                        for (const auto& t : proc.threads) {
                            if (t.tid == ev.tid) {
                                ImGui::TextUnformatted(t.name.c_str());
                                found_name = true;
                                break;
                            }
                        }
                        break;
                    }
                    if (!found_name) {
                        ImGui::Text("%u:%u", ev.pid, ev.tid);
                    }
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();
}
