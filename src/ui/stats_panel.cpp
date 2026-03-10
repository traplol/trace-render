#include "stats_panel.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>

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

void StatsPanel::select_function_by_name(const std::string& name, const TraceModel& model) {
    selected_name_ = name;
    instances_.clear();
    instance_cursor_ = -1;

    for (uint32_t i = 0; i < (uint32_t)model.events_.size(); i++) {
        const auto& ev = model.events_[i];
        if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
            continue;
        if (ev.dur <= 0) continue;
        if (model.get_string(ev.name_idx) == name) {
            instances_.push_back(i);
        }
    }

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

void StatsPanel::render(const TraceModel& model, QueryDb& db, ViewState& view) {
    ImGui::Begin("Statistics");

    if (!db.is_loaded()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    // Schema button
    ImGui::Text("Tables: events, processes, threads, counters");
    ImGui::SameLine();
    if (ImGui::SmallButton("Schema")) {
        result_ = db.execute(
            "SELECT 'events' as tbl, sql FROM sqlite_master WHERE name='events' "
            "UNION ALL SELECT 'processes', sql FROM sqlite_master WHERE name='processes' "
            "UNION ALL SELECT 'threads', sql FROM sqlite_master WHERE name='threads' "
            "UNION ALL SELECT 'counters', sql FROM sqlite_master WHERE name='counters'");
        has_result_ = true;
        selected_name_.clear();
        instances_.clear();
        instance_cursor_ = -1;
    }

    // SQL editor
    ImGui::InputTextMultiline("##sql", query_buf_, sizeof(query_buf_),
                               ImVec2(-1, ImGui::GetTextLineHeight() * 6));

    if (ImGui::Button("Run (Ctrl+Enter)") ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        result_ = db.execute(query_buf_);
        has_result_ = true;
        selected_name_.clear();
        instances_.clear();
        instance_cursor_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        has_result_ = false;
        result_ = {};
        selected_name_.clear();
        instances_.clear();
        instance_cursor_ = -1;
    }

    if (!has_result_) {
        ImGui::End();
        return;
    }

    if (!result_.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", result_.error.c_str());
    }

    if (!result_.ok || result_.columns.empty()) {
        ImGui::End();
        return;
    }

    ImGui::Text("%zu rows", result_.rows.size());

    // Find the "name" column index for click-to-browse
    int name_col = -1;
    for (int c = 0; c < (int)result_.columns.size(); c++) {
        if (result_.columns[c] == "name") {
            name_col = c;
            break;
        }
    }
    if (name_col >= 0 && !selected_name_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(click a name to browse instances)");
    }

    // Split: results table and instance browser
    float avail_h = ImGui::GetContentRegionAvail().y;
    float results_h = !selected_name_.empty() ? avail_h * 0.5f : 0.0f;

    ImGui::Separator();

    int col_count = (int)result_.columns.size();

    if (ImGui::BeginTable("QueryResults", col_count,
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable,
            ImVec2(0, results_h > 0 ? avail_h - results_h : 0))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        for (int c = 0; c < col_count; c++) {
            ImGui::TableSetupColumn(result_.columns[c].c_str(), ImGuiTableColumnFlags_None, 0.0f, (ImGuiID)c);
        }
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty) {
                sort_specs->SpecsDirty = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    int sort_col = (int)spec.ColumnUserID;
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(result_.rows.begin(), result_.rows.end(),
                        [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                            if (sort_col >= (int)a.size() || sort_col >= (int)b.size()) return false;
                            const std::string& sa = a[sort_col];
                            const std::string& sb = b[sort_col];
                            // Try numeric comparison first
                            char* end_a = nullptr;
                            char* end_b = nullptr;
                            double da = strtod(sa.c_str(), &end_a);
                            double db = strtod(sb.c_str(), &end_b);
                            int cmp;
                            if (end_a != sa.c_str() && *end_a == '\0' &&
                                end_b != sb.c_str() && *end_b == '\0') {
                                cmp = (da < db) ? -1 : (da > db) ? 1 : 0;
                            } else {
                                cmp = sa.compare(sb);
                            }
                            return asc ? (cmp < 0) : (cmp > 0);
                        });
                }
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)result_.rows.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& row = result_.rows[i];
                ImGui::TableNextRow();

                for (int c = 0; c < col_count && c < (int)row.size(); c++) {
                    ImGui::TableNextColumn();

                    if (c == name_col) {
                        // Clickable name cell
                        char id_buf[32];
                        snprintf(id_buf, sizeof(id_buf), "##r%d", i);
                        bool is_selected = (row[c] == selected_name_);
                        if (ImGui::Selectable(id_buf, is_selected,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                            select_function_by_name(row[c], model);
                            if (!instances_.empty()) {
                                navigate_to_instance(0, model, view);
                            }
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(row[c].c_str());
                        // Skip remaining columns since SpanAllColumns handles the click
                        for (int cc = c + 1; cc < col_count && cc < (int)row.size(); cc++) {
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(row[cc].c_str());
                        }
                        break;
                    } else {
                        ImGui::TextUnformatted(row[c].c_str());
                    }
                }
            }
        }

        ImGui::EndTable();
    }

    // Instance browser
    if (!selected_name_.empty() && !instances_.empty()) {
        ImGui::Separator();

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
        ImGui::SameLine();
        if (ImGui::Button("Close##instances")) {
            selected_name_.clear();
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
