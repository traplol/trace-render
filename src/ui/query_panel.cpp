#include "query_panel.h"
#include "imgui.h"
#include <cstdio>

void QueryPanel::render(QueryDb& db, ViewState& view) {
    ImGui::Begin("SQL Query");

    if (!db.is_loaded()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    // Query input
    ImGui::Text("Tables: events, processes, threads, counters");
    ImGui::SameLine();
    if (ImGui::SmallButton("Schema")) {
        result_ = db.execute(
            "SELECT 'events' as tbl, sql FROM sqlite_master WHERE name='events' "
            "UNION ALL SELECT 'processes', sql FROM sqlite_master WHERE name='processes' "
            "UNION ALL SELECT 'threads', sql FROM sqlite_master WHERE name='threads' "
            "UNION ALL SELECT 'counters', sql FROM sqlite_master WHERE name='counters'");
        has_result_ = true;
    }

    ImGui::InputTextMultiline("##sql", query_buf_, sizeof(query_buf_),
                               ImVec2(-1, ImGui::GetTextLineHeight() * 6));

    if (ImGui::Button("Run (Ctrl+Enter)") ||
        (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        result_ = db.execute(query_buf_);
        has_result_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Results")) {
        has_result_ = false;
        result_ = {};
    }

    if (has_result_) {
        if (!result_.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", result_.error.c_str());
        }

        if (result_.ok && !result_.columns.empty()) {
            ImGui::Text("%zu rows", result_.rows.size());
            ImGui::Separator();

            int col_count = (int)result_.columns.size();

            if (ImGui::BeginTable("QueryResults", col_count,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_Reorderable,
                    ImVec2(0, 0))) {

                ImGui::TableSetupScrollFreeze(0, 1);
                for (int c = 0; c < col_count; c++) {
                    ImGui::TableSetupColumn(result_.columns[c].c_str());
                }
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin((int)result_.rows.size());
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                        const auto& row = result_.rows[i];
                        ImGui::TableNextRow();
                        for (int c = 0; c < col_count && c < (int)row.size(); c++) {
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(row[c].c_str());
                        }
                    }
                }

                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
}
