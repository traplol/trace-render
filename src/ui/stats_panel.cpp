#include "stats_panel.h"
#include "tracing.h"
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

nlohmann::json StatsPanel::save_tabs() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tab : tabs_) {
        nlohmann::json t;
        t["title"] = tab.title;
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
    TRACE_SCOPE_CAT("Statistics", "ui");
    ImGui::Begin("Statistics");

    if (!db.is_loaded()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    ensure_default_tab();

    // Tab bar with + button
    int tab_to_remove = -1;
    if (ImGui::BeginTabBar("QueryTabs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable)) {
        for (int t = 0; t < (int)tabs_.size(); t++) {
            bool open = true;
            bool* p_open = (tabs_.size() > 1) ? &open : nullptr;

            char tab_label[128];
            snprintf(tab_label, sizeof(tab_label), "%s###tab%d", tabs_[t].title.c_str(), t);

            if (ImGui::BeginTabItem(tab_label, p_open)) {
                active_tab_ = t;
                ImGui::BeginChild("##tabcontent", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
                render_tab(tabs_[t], model, db, view);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (p_open && !open) {
                tab_to_remove = t;
            }
        }

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

    render_schema_popup(db);

    ImGui::End();
}

void StatsPanel::render_schema_popup(QueryDb& db) {
    if (show_schema_) {
        ImGui::OpenPopup("Schema Browser");
        show_schema_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Schema Browser", nullptr, ImGuiWindowFlags_None)) {
        struct TableDef {
            const char* name;
            const char* columns; // name:type pairs
        };

        static const TableDef tables[] = {
            {"events", nullptr},
            {"processes", nullptr},
            {"threads", nullptr},
            {"counters", nullptr},
        };

        struct ColInfo {
            std::string name;
            std::string type;
        };

        // Query PRAGMA for each table and display
        for (const auto& tbl : tables) {
            if (ImGui::CollapsingHeader(tbl.name, ImGuiTreeNodeFlags_DefaultOpen)) {
                // Get column info
                auto result = db.execute(
                    std::string("PRAGMA table_info(") + tbl.name + ")");

                if (result.ok && !result.rows.empty()) {
                    // PRAGMA table_info columns: cid, name, type, notnull, dflt_value, pk
                    if (ImGui::BeginTable(tbl.name, 4,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Column", ImGuiTableColumnFlags_None, 3.0f);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_None, 2.0f);
                        ImGui::TableSetupColumn("PK", ImGuiTableColumnFlags_None, 0.5f);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_None, 1.5f);
                        ImGui::TableHeadersRow();

                        for (const auto& row : result.rows) {
                            // row: [cid, name, type, notnull, dflt_value, pk]
                            if (row.size() < 6) continue;
                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", row[1].c_str());

                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", row[2].c_str());

                            ImGui::TableNextColumn();
                            if (row[5] != "0") {
                                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "PK");
                            }

                            ImGui::TableNextColumn();
                            char btn_id[64];
                            snprintf(btn_id, sizeof(btn_id), "Copy##%s_%s", tbl.name, row[1].c_str());
                            if (ImGui::SmallButton(btn_id)) {
                                ImGui::SetClipboardText(row[1].c_str());
                            }
                        }
                        ImGui::EndTable();
                    }

                    // Row count
                    auto count_result = db.execute(
                        std::string("SELECT COUNT(*) FROM ") + tbl.name);
                    if (count_result.ok && !count_result.rows.empty()) {
                        ImGui::TextDisabled("  %s rows", count_result.rows[0][0].c_str());
                    }
                }

                ImGui::Spacing();
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Example queries section
        if (ImGui::CollapsingHeader("Example Queries")) {
            struct Example {
                const char* name;
                const char* sql;
            };
            static const Example examples[] = {
                {"Hot Functions",
                 "SELECT name, COUNT(*) as count, SUM(dur) as total_dur, AVG(dur) as avg_dur\n"
                 "FROM events WHERE dur > 0\n"
                 "GROUP BY name ORDER BY total_dur DESC LIMIT 50"},
                {"Longest Events",
                 "SELECT name, ts, dur, pid, tid\n"
                 "FROM events ORDER BY dur DESC LIMIT 20"},
                {"Events per Thread",
                 "SELECT t.name as thread, p.name as process, COUNT(*) as count\n"
                 "FROM events e\n"
                 "JOIN threads t ON e.tid = t.tid AND e.pid = t.pid\n"
                 "JOIN processes p ON e.pid = p.pid\n"
                 "GROUP BY e.pid, e.tid ORDER BY count DESC"},
                {"Category Breakdown",
                 "SELECT category, COUNT(*) as count, SUM(dur) as total_dur\n"
                 "FROM events WHERE dur > 0\n"
                 "GROUP BY category ORDER BY total_dur DESC"},
                {"Counter Summary",
                 "SELECT name, pid, COUNT(*) as points,\n"
                 "  MIN(value) as min_val, MAX(value) as max_val, AVG(value) as avg_val\n"
                 "FROM counters GROUP BY name, pid"},
            };

            for (const auto& ex : examples) {
                ImGui::PushID(ex.name);
                if (ImGui::SmallButton("Use")) {
                    if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size()) {
                        snprintf(tabs_[active_tab_].query_buf,
                                sizeof(tabs_[active_tab_].query_buf), "%s", ex.sql);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                ImGui::Text("%s", ex.name);
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(200, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void StatsPanel::render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view) {
    if (tab.query_buf_dirty) {
        tab.query_buf_dirty = false;
        snprintf(tab.query_buf, sizeof(tab.query_buf), "%s", tab.query.c_str());
    }

    // Schema button
    if (ImGui::SmallButton("Schema")) {
        show_schema_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Tables: events, processes, threads, counters");

    // Rename tab
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s", tab.title.c_str());
    if (ImGui::InputText("##tabtitle", title_buf, sizeof(title_buf))) {
        tab.title = title_buf;
    }

    // Check if async query completed
    if (tab.query_running && db.is_query_done()) {
        tab.result = db.take_result();
        tab.has_result = true;
        tab.query_running = false;
    }

    // SQL editor
    bool is_running = tab.query_running;
    if (is_running) {
        ImGui::BeginDisabled();
    }
    ImGui::InputTextMultiline("##sql", tab.query_buf, sizeof(tab.query_buf),
                               ImVec2(-1, ImGui::GetTextLineHeight() * 6));
    if (is_running) {
        ImGui::EndDisabled();
    }

    if (is_running) {
        // Show progress while query runs
        if (ImGui::Button("Cancel")) {
            db.cancel_query();
        }
        ImGui::SameLine();

        float time = (float)ImGui::GetTime();
        const char* spinner[] = { "|", "/", "-", "\\" };
        int frame = (int)(time * 4.0f) % 4;
        int rows = db.query_rows_so_far();
        uint64_t steps = db.query_steps();

        char status[128];
        if (steps < 1000000) {
            snprintf(status, sizeof(status), "%s  Running... %d rows, %lluK steps",
                     spinner[frame], rows, (unsigned long long)(steps / 1000));
        } else {
            snprintf(status, sizeof(status), "%s  Running... %d rows, %.1fM steps",
                     spinner[frame], rows, steps / 1000000.0);
        }
        ImGui::TextDisabled("%s", status);
    } else {
        bool run = ImGui::Button("Run (Ctrl+Enter)") ||
            (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter));
        if (run && !db.is_query_running()) {
            db.execute_async(tab.query_buf);
            tab.query_running = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            tab.has_result = false;
            tab.result = {};
        }
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
    if (name_col >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(click a name to browse instances)");
    }

    ImGui::Separator();

    int col_count = (int)tab.result.columns.size();

    if (ImGui::BeginTable("QueryResults", col_count,
            ImGuiTableFlags_Sortable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable,
            ImVec2(0, 0))) {

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
                        if (ImGui::Selectable(id_buf, false,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                            // Find first event with this name and select it
                            const std::string& name = row[name_col];
                            for (uint32_t ei = 0; ei < (uint32_t)model.events_.size(); ei++) {
                                const auto& ev = model.events_[ei];
                                if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
                                    continue;
                                if (ev.dur <= 0) continue;
                                if (model.get_string(ev.name_idx) == name) {
                                    view.selected_event_idx = ei;
                                    break;
                                }
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
}
