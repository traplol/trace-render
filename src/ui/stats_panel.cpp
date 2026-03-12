#include "stats_panel.h"
#include "format_time.h"
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
            format_time(v, buf, sizeof(buf));
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
                ImGui::BeginChild("##tabcontent", ImVec2(0, 0), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar);
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
    render_builder_popup(db);

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
            const char* columns;  // name:type pairs
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
                auto result = db.execute(std::string("PRAGMA table_info(") + tbl.name + ")");

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
                    auto count_result = db.execute(std::string("SELECT COUNT(*) FROM ") + tbl.name);
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
                        snprintf(tabs_[active_tab_].query_buf, sizeof(tabs_[active_tab_].query_buf), "%s", ex.sql);
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

    // Schema & Builder buttons
    if (ImGui::SmallButton("Schema")) {
        show_schema_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Builder")) {
        show_builder_ = true;
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
        const char* spinner[] = {"|", "/", "-", "\\"};
        int frame = (int)(time * 4.0f) % 4;
        int rows = db.query_rows_so_far();
        uint64_t steps = db.query_steps();

        char status[128];
        if (steps < 1000000) {
            snprintf(status, sizeof(status), "%s  Running... %d rows, %lluK steps", spinner[frame], rows,
                     (unsigned long long)(steps / 1000));
        } else {
            snprintf(status, sizeof(status), "%s  Running... %d rows, %.1fM steps", spinner[frame], rows,
                     steps / 1000000.0);
        }
        ImGui::TextDisabled("%s", status);
    } else {
        bool run = ImGui::Button("Run (Ctrl+Enter)") || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter));
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
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable,
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
                                  if (end_a != sa.c_str() && *end_a == '\0' && end_b != sb.c_str() && *end_b == '\0') {
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
                        if (ImGui::Selectable(
                                id_buf, false,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                            // Find first event with this name and select it
                            const std::string& name = row[name_col];
                            for (uint32_t ei = 0; ei < (uint32_t)model.events_.size(); ei++) {
                                const auto& ev = model.events_[ei];
                                if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter) continue;
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

// --- Query Builder ---

static const char* TABLE_NAMES[] = {"events", "processes", "threads", "counters"};
static const int NUM_TABLES = 4;

static const char* EVENTS_COLS[] = {"id", "name", "category", "phase", "ts", "dur", "end_ts", "pid", "tid", "depth"};
static const char* PROCESSES_COLS[] = {"pid", "name"};
static const char* THREADS_COLS[] = {"tid", "pid", "name"};
static const char* COUNTERS_COLS[] = {"pid", "name", "ts", "value"};

static const char* const* TABLE_COLS[] = {EVENTS_COLS, PROCESSES_COLS, THREADS_COLS, COUNTERS_COLS};
static const int TABLE_COL_COUNTS[] = {10, 2, 3, 4};

static const char* AGG_NAMES[] = {"(none)", "COUNT", "SUM", "AVG", "MIN", "MAX"};
static const int NUM_AGGS = 6;

static const char* OP_NAMES[] = {"=", "!=", "<", ">", "<=", ">=", "LIKE", "NOT LIKE", "IN", "IS NULL", "IS NOT NULL"};
static const int NUM_OPS = 11;

static const char* LOGIC_NAMES[] = {"AND", "OR"};

void QueryBuilderState::reset() {
    table_idx = 0;
    select_cols.clear();
    where_clauses.clear();
    group_cols.clear();
    having_clauses.clear();
    order_cols.clear();
    use_limit = true;
    limit_value = 50;
}

std::string QueryBuilderState::build_sql(const char* const* columns, int num_columns) const {
    std::string sql = "SELECT ";

    // SELECT
    if (select_cols.empty()) {
        sql += "*";
    } else {
        for (int i = 0; i < (int)select_cols.size(); i++) {
            if (i > 0) sql += ", ";
            const auto& sc = select_cols[i];
            const char* col = (sc.col_idx >= 0 && sc.col_idx < num_columns) ? columns[sc.col_idx] : "?";
            if (sc.agg_idx > 0 && sc.agg_idx < NUM_AGGS) {
                sql += AGG_NAMES[sc.agg_idx];
                sql += "(";
                sql += col;
                sql += ")";
            } else {
                sql += col;
            }
            if (sc.alias[0]) {
                sql += " AS ";
                sql += sc.alias;
            }
        }
    }

    // FROM
    sql += "\nFROM ";
    sql += TABLE_NAMES[table_idx];

    // WHERE
    if (!where_clauses.empty()) {
        sql += "\nWHERE ";
        for (int i = 0; i < (int)where_clauses.size(); i++) {
            const auto& wc = where_clauses[i];
            if (i > 0) {
                sql += " ";
                sql += LOGIC_NAMES[wc.logic_idx];
                sql += " ";
            }
            const char* col = (wc.col_idx >= 0 && wc.col_idx < num_columns) ? columns[wc.col_idx] : "?";
            sql += col;
            sql += " ";
            sql += OP_NAMES[wc.op_idx];
            // IS NULL / IS NOT NULL don't need a value
            if (wc.op_idx < 9) {
                sql += " ";
                sql += wc.value;
            }
        }
    }

    // GROUP BY
    if (!group_cols.empty()) {
        sql += "\nGROUP BY ";
        for (int i = 0; i < (int)group_cols.size(); i++) {
            if (i > 0) sql += ", ";
            int ci = group_cols[i];
            sql += (ci >= 0 && ci < num_columns) ? columns[ci] : "?";
        }
    }

    // HAVING
    if (!group_cols.empty() && !having_clauses.empty()) {
        sql += "\nHAVING ";
        for (int i = 0; i < (int)having_clauses.size(); i++) {
            const auto& hc = having_clauses[i];
            if (i > 0) sql += " AND ";
            const char* col = (hc.col_idx >= 0 && hc.col_idx < num_columns) ? columns[hc.col_idx] : "?";
            if (hc.agg_idx > 0 && hc.agg_idx < NUM_AGGS) {
                sql += AGG_NAMES[hc.agg_idx];
                sql += "(";
                sql += col;
                sql += ")";
            } else {
                sql += col;
            }
            sql += " ";
            sql += OP_NAMES[hc.op_idx];
            if (hc.op_idx < 9) {
                sql += " ";
                sql += hc.value;
            }
        }
    }

    // ORDER BY
    if (!order_cols.empty()) {
        sql += "\nORDER BY ";
        for (int i = 0; i < (int)order_cols.size(); i++) {
            if (i > 0) sql += ", ";
            const auto& oc = order_cols[i];
            // Check if it matches a SELECT alias
            bool found_alias = false;
            if (oc.col_idx >= 0 && oc.col_idx < (int)select_cols.size() && select_cols[oc.col_idx].alias[0]) {
                sql += select_cols[oc.col_idx].alias;
                found_alias = true;
            }
            if (!found_alias) {
                // Use column index into the table
                int ci = oc.col_idx;
                sql += (ci >= 0 && ci < num_columns) ? columns[ci] : "?";
            }
            sql += oc.descending ? " DESC" : " ASC";
        }
    }

    // LIMIT
    if (use_limit && limit_value > 0) {
        sql += "\nLIMIT ";
        sql += std::to_string(limit_value);
    }

    return sql;
}

void StatsPanel::render_builder_popup(QueryDb& db) {
    if (show_builder_) {
        ImGui::OpenPopup("Query Builder");
        show_builder_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Query Builder", nullptr, ImGuiWindowFlags_None)) return;

    auto& b = builder_;
    const char* const* cols = TABLE_COLS[b.table_idx];
    int num_cols = TABLE_COL_COUNTS[b.table_idx];

    // --- TABLE ---
    ImGui::SeparatorText("FROM");
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Table", &b.table_idx, TABLE_NAMES, NUM_TABLES)) {
        // Reset column references when table changes
        b.select_cols.clear();
        b.where_clauses.clear();
        b.group_cols.clear();
        b.having_clauses.clear();
        b.order_cols.clear();
        cols = TABLE_COLS[b.table_idx];
        num_cols = TABLE_COL_COUNTS[b.table_idx];
    }

    // --- SELECT ---
    ImGui::SeparatorText("SELECT");
    if (b.select_cols.empty()) {
        ImGui::TextDisabled("* (all columns) - add columns to customize");
    }

    int sel_to_remove = -1;
    for (int i = 0; i < (int)b.select_cols.size(); i++) {
        ImGui::PushID(i);
        auto& sc = b.select_cols[i];

        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##col", &sc.col_idx, cols, num_cols);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(100);
        ImGui::Combo("##agg", &sc.agg_idx, AGG_NAMES, NUM_AGGS);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##alias", "alias", sc.alias, sizeof(sc.alias));
        ImGui::SameLine();

        if (ImGui::SmallButton("X")) sel_to_remove = i;

        ImGui::PopID();
    }
    if (sel_to_remove >= 0) b.select_cols.erase(b.select_cols.begin() + sel_to_remove);

    if (ImGui::SmallButton("+ Column")) {
        b.select_cols.push_back({});
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ All Columns")) {
        b.select_cols.clear();
        for (int i = 0; i < num_cols; i++) {
            QueryBuilderState::SelectCol sc;
            sc.col_idx = i;
            b.select_cols.push_back(sc);
        }
    }

    // --- WHERE ---
    ImGui::SeparatorText("WHERE");

    int where_to_remove = -1;
    for (int i = 0; i < (int)b.where_clauses.size(); i++) {
        ImGui::PushID(1000 + i);
        auto& wc = b.where_clauses[i];

        if (i > 0) {
            ImGui::SetNextItemWidth(60);
            ImGui::Combo("##logic", &wc.logic_idx, LOGIC_NAMES, 2);
            ImGui::SameLine();
        }

        ImGui::SetNextItemWidth(120);
        ImGui::Combo("##wcol", &wc.col_idx, cols, num_cols);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##wop", &wc.op_idx, OP_NAMES, NUM_OPS);

        // No value input for IS NULL / IS NOT NULL
        if (wc.op_idx < 9) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            const char* hint = (wc.op_idx == 6 || wc.op_idx == 7) ? "%pattern%"
                               : (wc.op_idx == 8)                 ? "(1,2,3)"
                                                                  : "value";
            ImGui::InputTextWithHint("##wval", hint, wc.value, sizeof(wc.value));
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) where_to_remove = i;

        ImGui::PopID();
    }
    if (where_to_remove >= 0) b.where_clauses.erase(b.where_clauses.begin() + where_to_remove);

    if (ImGui::SmallButton("+ Condition")) {
        b.where_clauses.push_back({});
    }

    // --- GROUP BY ---
    ImGui::SeparatorText("GROUP BY");

    int grp_to_remove = -1;
    for (int i = 0; i < (int)b.group_cols.size(); i++) {
        ImGui::PushID(2000 + i);
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##gcol", &b.group_cols[i], cols, num_cols);
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) grp_to_remove = i;
        ImGui::PopID();
    }
    if (grp_to_remove >= 0) b.group_cols.erase(b.group_cols.begin() + grp_to_remove);

    if (ImGui::SmallButton("+ Group Column")) {
        b.group_cols.push_back(0);
    }

    // --- HAVING (only when GROUP BY is active) ---
    if (!b.group_cols.empty()) {
        ImGui::SeparatorText("HAVING");

        int hav_to_remove = -1;
        for (int i = 0; i < (int)b.having_clauses.size(); i++) {
            ImGui::PushID(3000 + i);
            auto& hc = b.having_clauses[i];

            ImGui::SetNextItemWidth(100);
            // Skip "(none)" for HAVING - start at index 1
            const char* having_aggs[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
            int disp_agg = hc.agg_idx - 1;
            if (disp_agg < 0) disp_agg = 0;
            if (ImGui::Combo("##hagg", &disp_agg, having_aggs, 5)) {
                hc.agg_idx = disp_agg + 1;
            }
            ImGui::SameLine();

            ImGui::SetNextItemWidth(120);
            ImGui::Combo("##hcol", &hc.col_idx, cols, num_cols);
            ImGui::SameLine();

            ImGui::SetNextItemWidth(80);
            ImGui::Combo("##hop", &hc.op_idx, OP_NAMES, 6);  // Only comparison ops
            ImGui::SameLine();

            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##hval", "value", hc.value, sizeof(hc.value));
            ImGui::SameLine();

            if (ImGui::SmallButton("X")) hav_to_remove = i;

            ImGui::PopID();
        }
        if (hav_to_remove >= 0) b.having_clauses.erase(b.having_clauses.begin() + hav_to_remove);

        if (ImGui::SmallButton("+ Having Condition")) {
            QueryBuilderState::HavingClause hc;
            hc.agg_idx = 1;
            b.having_clauses.push_back(hc);
        }
    }

    // --- ORDER BY ---
    ImGui::SeparatorText("ORDER BY");

    int ord_to_remove = -1;
    for (int i = 0; i < (int)b.order_cols.size(); i++) {
        ImGui::PushID(4000 + i);
        auto& oc = b.order_cols[i];

        // Show either table columns or SELECT aliases
        if (!b.select_cols.empty()) {
            // Build label list from select cols
            std::vector<std::string> labels;
            for (const auto& sc : b.select_cols) {
                std::string lbl;
                if (sc.agg_idx > 0) {
                    lbl += AGG_NAMES[sc.agg_idx];
                    lbl += "(";
                    lbl += (sc.col_idx >= 0 && sc.col_idx < num_cols) ? cols[sc.col_idx] : "?";
                    lbl += ")";
                } else {
                    lbl = (sc.col_idx >= 0 && sc.col_idx < num_cols) ? cols[sc.col_idx] : "?";
                }
                if (sc.alias[0]) {
                    lbl += " [";
                    lbl += sc.alias;
                    lbl += "]";
                }
                labels.push_back(lbl);
            }
            // ImGui combo from string vector
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("##ocol", oc.col_idx < (int)labels.size() ? labels[oc.col_idx].c_str() : "?")) {
                for (int j = 0; j < (int)labels.size(); j++) {
                    if (ImGui::Selectable(labels[j].c_str(), j == oc.col_idx)) {
                        oc.col_idx = j;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::SetNextItemWidth(150);
            ImGui::Combo("##ocol", &oc.col_idx, cols, num_cols);
        }
        ImGui::SameLine();

        const char* dir_items[] = {"ASC", "DESC"};
        int dir = oc.descending ? 1 : 0;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##odir", &dir, dir_items, 2)) {
            oc.descending = (dir == 1);
        }
        ImGui::SameLine();

        if (ImGui::SmallButton("X")) ord_to_remove = i;

        ImGui::PopID();
    }
    if (ord_to_remove >= 0) b.order_cols.erase(b.order_cols.begin() + ord_to_remove);

    if (ImGui::SmallButton("+ Order Column")) {
        b.order_cols.push_back({});
    }

    // --- LIMIT ---
    ImGui::SeparatorText("LIMIT");
    ImGui::Checkbox("Enable limit", &b.use_limit);
    if (b.use_limit) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##limit", &b.limit_value);
        if (b.limit_value < 1) b.limit_value = 1;
    }

    // --- SQL Preview ---
    ImGui::SeparatorText("Generated SQL");
    std::string sql = b.build_sql(cols, num_cols);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
    ImGui::TextWrapped("%s", sql.c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Action buttons
    if (ImGui::Button("Use Query", ImVec2(200, 0))) {
        if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size()) {
            snprintf(tabs_[active_tab_].query_buf, sizeof(tabs_[active_tab_].query_buf), "%s", sql.c_str());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Use & Run", ImVec2(200, 0))) {
        if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size()) {
            snprintf(tabs_[active_tab_].query_buf, sizeof(tabs_[active_tab_].query_buf), "%s", sql.c_str());
            if (!db.is_query_running()) {
                db.execute_async(sql);
                tabs_[active_tab_].query_running = true;
            }
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy SQL", ImVec2(150, 0))) {
        ImGui::SetClipboardText(sql.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(100, 0))) {
        b.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
