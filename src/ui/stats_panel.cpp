#include "stats_panel.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

static bool is_time_column(const std::string& name) {
    if (name.find("dur") != std::string::npos) return true;
    if (name == "ts" || name == "min_ts" || name == "max_ts") return true;
    if (name.find("time") != std::string::npos) return true;
    return false;
}

static void render_cell(const std::string& value, bool is_time) {
    if (is_time) {
        char* end = nullptr;
        double v = strtod(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0') {
            char buf[64];
            double abs_v = std::abs(v);
            if (abs_v < 1.0)
                snprintf(buf, sizeof(buf), "%.1f ns", v * 1000.0);
            else if (abs_v < 1000.0)
                snprintf(buf, sizeof(buf), "%.3f us", v);
            else if (abs_v < 1000000.0)
                snprintf(buf, sizeof(buf), "%.3f ms", v / 1000.0);
            else
                snprintf(buf, sizeof(buf), "%.3f s", v / 1000000.0);
            ImGui::TextUnformatted(buf);
            return;
        }
    }
    ImGui::TextUnformatted(value.c_str());
}

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

void StatsPanel::ensure_default_tab() {
    if (tabs_.empty()) {
        QueryTab tab;
        tab.title = "Hot Functions";
        tab.query = DEFAULT_QUERY;
        tab.query_buf_dirty = true;
        tabs_.push_back(std::move(tab));
        active_tab_ = 0;
    }
}

void StatsPanel::select_function_by_name(QueryTab& tab, const std::string& name, const TraceModel& model) {
    tab.selected_name = name;
    tab.instances.clear();
    tab.instance_cursor = -1;

    for (uint32_t i = 0; i < (uint32_t)model.events_.size(); i++) {
        const auto& ev = model.events_[i];
        if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
            continue;
        if (ev.dur <= 0) continue;
        if (model.get_string(ev.name_idx) == name) {
            tab.instances.push_back(i);
        }
    }

    std::sort(tab.instances.begin(), tab.instances.end(),
        [&](uint32_t a, uint32_t b) {
            return model.events_[a].ts < model.events_[b].ts;
        });
}

void StatsPanel::navigate_to_instance(QueryTab& tab, int32_t idx, const TraceModel& model, ViewState& view) {
    if (idx < 0 || idx >= (int32_t)tab.instances.size()) return;
    tab.instance_cursor = idx;
    uint32_t ev_idx = tab.instances[idx];
    view.selected_event_idx = ev_idx;
    const auto& ev = model.events_[ev_idx];
    double pad = std::max(ev.dur * 0.5, 100.0);
    view.view_start_ts = ev.ts - pad;
    view.view_end_ts = ev.end_ts() + pad;
}

nlohmann::json StatsPanel::save_tabs() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tab : tabs_) {
        nlohmann::json t;
        t["title"] = tab.title;
        // Save the live buffer content if it has been touched, otherwise the stored query
        if (!tab.query_buf_dirty) {
            t["query"] = std::string(tab.query_buf);
        } else {
            t["query"] = tab.query;
        }
        arr.push_back(t);
    }
    return arr;
}

void StatsPanel::load_tabs(const nlohmann::json& j) {
    tabs_.clear();
    if (!j.is_array()) return;
    for (const auto& item : j) {
        QueryTab tab;
        if (item.contains("title")) tab.title = item["title"].get<std::string>();
        if (item.contains("query")) tab.query = item["query"].get<std::string>();
        tab.query_buf_dirty = true;
        tabs_.push_back(std::move(tab));
    }
    active_tab_ = 0;
}

void StatsPanel::render(const TraceModel& model, QueryDb& db, ViewState& view) {
    ImGui::Begin("Statistics");

    if (!db.is_loaded()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    ensure_default_tab();

    // Track external selection changes and update active tab's instance browser
    if (view.selected_event_idx != last_selected_event_ && view.selected_event_idx >= 0) {
        last_selected_event_ = view.selected_event_idx;
        const auto& ev = model.events_[view.selected_event_idx];
        if (!ev.is_end_event && ev.ph != Phase::Metadata && ev.ph != Phase::Counter) {
            const std::string& name = model.get_string(ev.name_idx);
            if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size()) {
                auto& tab = tabs_[active_tab_];
                if (tab.selected_name != name) {
                    select_function_by_name(tab, name, model);
                }
                // Set cursor to the selected event
                for (int i = 0; i < (int)tab.instances.size(); i++) {
                    if (tab.instances[i] == (uint32_t)view.selected_event_idx) {
                        tab.instance_cursor = i;
                        break;
                    }
                }
            }
        }
    } else if (view.selected_event_idx < 0 && last_selected_event_ >= 0) {
        last_selected_event_ = -1;
    }

    // Tab bar with + button
    int tab_to_remove = -1;
    if (ImGui::BeginTabBar("QueryTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable)) {
        for (int t = 0; t < (int)tabs_.size(); t++) {
            bool open = true;
            // Don't allow closing the last tab
            bool* p_open = (tabs_.size() > 1) ? &open : nullptr;

            char tab_label[128];
            snprintf(tab_label, sizeof(tab_label), "%s###tab%d", tabs_[t].title.c_str(), t);

            if (ImGui::BeginTabItem(tab_label, p_open)) {
                active_tab_ = t;
                render_tab(tabs_[t], model, db, view);
                ImGui::EndTabItem();
            }

            if (p_open && !open) {
                tab_to_remove = t;
            }
        }

        // "+" button to add new tab
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            QueryTab tab;
            tab.title = "Query " + std::to_string(tabs_.size() + 1);
            tab.query = DEFAULT_QUERY;
            tab.query_buf_dirty = true;
            tabs_.push_back(std::move(tab));
        }

        ImGui::EndTabBar();
    }

    if (tab_to_remove >= 0) {
        tabs_.erase(tabs_.begin() + tab_to_remove);
        if (active_tab_ >= (int)tabs_.size()) active_tab_ = (int)tabs_.size() - 1;
    }

    ImGui::End();
}

void StatsPanel::render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view) {
    // Sync query string -> buffer on first render or after load
    if (tab.query_buf_dirty) {
        tab.query_buf_dirty = false;
        snprintf(tab.query_buf, sizeof(tab.query_buf), "%s", tab.query.c_str());
    }

    // Schema button
    ImGui::Text("Tables: events, processes, threads, counters");
    ImGui::SameLine();
    if (ImGui::SmallButton("Schema")) {
        tab.result = db.execute(
            "SELECT 'events' as tbl, sql FROM sqlite_master WHERE name='events' "
            "UNION ALL SELECT 'processes', sql FROM sqlite_master WHERE name='processes' "
            "UNION ALL SELECT 'threads', sql FROM sqlite_master WHERE name='threads' "
            "UNION ALL SELECT 'counters', sql FROM sqlite_master WHERE name='counters'");
        tab.has_result = true;
        tab.selected_name.clear();
        tab.instances.clear();
        tab.instance_cursor = -1;
    }

    // Rename tab
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s", tab.title.c_str());
    if (ImGui::InputText("##tabtitle", title_buf, sizeof(title_buf))) {
        tab.title = title_buf;
    }

    // SQL editor
    ImGui::InputTextMultiline("##sql", tab.query_buf, sizeof(tab.query_buf),
                               ImVec2(-1, ImGui::GetTextLineHeight() * 6));

    if (ImGui::Button("Run (Ctrl+Enter)") ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        tab.result = db.execute(tab.query_buf);
        tab.has_result = true;
        tab.selected_name.clear();
        tab.instances.clear();
        tab.instance_cursor = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        tab.has_result = false;
        tab.result = {};
        tab.selected_name.clear();
        tab.instances.clear();
        tab.instance_cursor = -1;
    }

    if (!tab.has_result) return;

    if (!tab.result.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tab.result.error.c_str());
    }

    if (!tab.result.ok || tab.result.columns.empty()) return;

    ImGui::Text("%zu rows", tab.result.rows.size());

    // Find the "name" column index for click-to-browse
    int name_col = -1;
    for (int c = 0; c < (int)tab.result.columns.size(); c++) {
        if (tab.result.columns[c] == "name") {
            name_col = c;
            break;
        }
    }
    if (name_col >= 0 && !tab.selected_name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(click a name to browse instances)");
    }

    // Split: results table and instance browser
    float avail_h = ImGui::GetContentRegionAvail().y;
    float results_h = !tab.selected_name.empty() ? avail_h * 0.5f : 0.0f;

    ImGui::Separator();

    int col_count = (int)tab.result.columns.size();

    if (ImGui::BeginTable("QueryResults", col_count,
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable,
            ImVec2(0, results_h > 0 ? avail_h - results_h : 0))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        std::vector<bool> time_cols(col_count, false);
        for (int c = 0; c < col_count; c++) {
            ImGui::TableSetupColumn(tab.result.columns[c].c_str(), ImGuiTableColumnFlags_None, 0.0f, (ImGuiID)c);
            time_cols[c] = is_time_column(tab.result.columns[c]);
        }
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty) {
                sort_specs->SpecsDirty = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    int sort_col = (int)spec.ColumnUserID;
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(tab.result.rows.begin(), tab.result.rows.end(),
                        [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                            if (sort_col >= (int)a.size() || sort_col >= (int)b.size()) return false;
                            const std::string& sa = a[sort_col];
                            const std::string& sb = b[sort_col];
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
        clipper.Begin((int)tab.result.rows.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& row = tab.result.rows[i];
                ImGui::TableNextRow();

                for (int c = 0; c < col_count && c < (int)row.size(); c++) {
                    ImGui::TableNextColumn();

                    if (c == name_col) {
                        char id_buf[32];
                        snprintf(id_buf, sizeof(id_buf), "##r%d", i);
                        bool is_selected = (row[c] == tab.selected_name);
                        if (ImGui::Selectable(id_buf, is_selected,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                            select_function_by_name(tab, row[c], model);
                            if (!tab.instances.empty()) {
                                navigate_to_instance(tab, 0, model, view);
                            }
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(row[c].c_str());
                        for (int cc = c + 1; cc < col_count && cc < (int)row.size(); cc++) {
                            ImGui::TableNextColumn();
                            render_cell(row[cc], time_cols[cc]);
                        }
                        break;
                    } else {
                        render_cell(row[c], time_cols[c]);
                    }
                }
            }
        }

        ImGui::EndTable();
    }

    // Instance browser
    if (!tab.selected_name.empty() && !tab.instances.empty()) {
        ImGui::Separator();

        ImGui::Text("Instances of \"%s\"", tab.selected_name.c_str());
        ImGui::SameLine();
        ImGui::Text("(%d / %zu)", tab.instance_cursor + 1, tab.instances.size());

        ImGui::SameLine();
        if (ImGui::Button("<##prev") && tab.instance_cursor > 0) {
            navigate_to_instance(tab, tab.instance_cursor - 1, model, view);
        }
        ImGui::SameLine();
        if (ImGui::Button(">##next") && tab.instance_cursor < (int32_t)tab.instances.size() - 1) {
            navigate_to_instance(tab, tab.instance_cursor + 1, model, view);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close##instances")) {
            tab.selected_name.clear();
            tab.instances.clear();
            tab.instance_cursor = -1;
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
                        std::sort(tab.instances.begin(), tab.instances.end(),
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
            clipper.Begin((int)tab.instances.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    uint32_t ev_idx = tab.instances[i];
                    const auto& ev = model.events_[ev_idx];

                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "%d##i%d", i + 1, i);
                    bool is_selected = (tab.instance_cursor == i);
                    if (ImGui::Selectable(id_buf, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        navigate_to_instance(tab, i, model, view);
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
}
