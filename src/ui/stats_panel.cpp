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
                render_tab(tabs_[t], model, db, view);
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

    ImGui::End();
}

void StatsPanel::render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view) {
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
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        tab.has_result = false;
        tab.result = {};
    }

    if (!tab.has_result) return;

    if (!tab.result.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", tab.result.error.c_str());
    }

    if (!tab.result.ok || tab.result.columns.empty()) return;

    ImGui::Text("%zu rows", tab.result.rows.size());
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
                    render_cell(row[c], time_cols[c]);
                }
            }
        }

        ImGui::EndTable();
    }
}
