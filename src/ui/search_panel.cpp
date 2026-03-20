#include "search_panel.h"
#include "format_time.h"
#include "sort_utils.h"
#include "string_utils.h"
#include "tracing.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

void SearchPanel::reset() {
    TRACE_FUNCTION_CAT("ui");
    search_buf_[0] = '\0';
    needs_search_ = false;
    sorted_results_.clear();
    needs_sort_ = false;
    scroll_to_top_ = false;
}

void SearchPanel::on_model_changed() {
    reset();
}

void SearchPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Search", "ui");
    ImGui::Begin("Search");

    if (view.key_bindings().is_pressed(Action::Search)) {
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::SetNextItemWidth(-60);
    if (ImGui::InputText("##search", search_buf_, sizeof(search_buf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        needs_search_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Find")) {
        needs_search_ = true;
    }

    if (needs_search_) {
        needs_search_ = false;
        view.set_search_query(search_buf_);
        view.clear_search_results();
        view.set_search_current(-1);

        if (!view.search_query().empty()) {
            for (uint32_t i = 0; i < model.events().size(); i++) {
                const auto& ev = model.events()[i];
                if (ev.is_end_event || ev.ph == Phase::Metadata) continue;
                const std::string& name = model.get_string(ev.name_idx);
                const std::string& cat = model.get_string(ev.cat_idx);
                if (contains_case_insensitive(name, view.search_query()) ||
                    contains_case_insensitive(cat, view.search_query())) {
                    view.add_search_result(i);
                }
            }
        }
        sorted_results_ = view.search_results();
        needs_sort_ = true;
    }

    ImGui::Text("%zu results", view.search_results().size());

    // Navigation
    bool navigate = false;
    if (!view.search_results().empty()) {
        ImGui::SameLine();
        if (ImGui::Button("<") && view.search_current() > 0) {
            view.set_search_current(view.search_current() - 1);
            navigate = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(">")) {
            view.set_search_current(view.search_current() + 1);
            if (view.search_current() >= (int32_t)view.search_results().size())
                view.set_search_current((int32_t)view.search_results().size() - 1);
            navigate = true;
        }

        if (navigate && view.search_current() >= 0 && view.search_current() < (int32_t)view.search_results().size()) {
            uint32_t ev_idx = view.search_results()[view.search_current()];
            view.navigate_to_event(ev_idx, model.events()[ev_idx], 2.0, 1000.0);
        }
    }

    ImGui::Separator();

    // Results table
    if (!sorted_results_.empty() &&
        ImGui::BeginTable("SearchResults", 3,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable,
                          ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 2);
        ImGui::TableHeadersRow();

        // Sort
        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (sort_specs->SpecsDirty || needs_sort_) {
                bool user_sorted = !needs_sort_;
                sort_specs->SpecsDirty = false;
                needs_sort_ = false;

                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    std::sort(sorted_results_.begin(), sorted_results_.end(), [&](uint32_t a_idx, uint32_t b_idx) {
                        const auto& a = model.events()[a_idx];
                        const auto& b = model.events()[b_idx];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0:  // Time
                                cmp = sort_utils::three_way_cmp(a.ts, b.ts);
                                break;
                            case 1:  // Duration
                                cmp = sort_utils::three_way_cmp(a.dur, b.dur);
                                break;
                            case 2: {  // Name
                                const auto& na = model.get_string(a.name_idx);
                                const auto& nb = model.get_string(b.name_idx);
                                cmp = na.compare(nb);
                                break;
                            }
                        }
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
                if (user_sorted) {
                    scroll_to_top_ = true;
                }
            }
        }

        if (scroll_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top_ = false;
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)sorted_results_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                uint32_t ev_idx = sorted_results_[i];
                const auto& ev = model.events()[ev_idx];
                const std::string& name = model.get_string(ev.name_idx);

                char time_buf[64];
                char dur_buf[64];
                format_time(ev.ts, time_buf, sizeof(time_buf));
                format_time(ev.dur, dur_buf, sizeof(dur_buf));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                // Make entire row selectable
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##r%d", i);
                bool selected = (view.selected_event_idx() == (int32_t)ev_idx);
                if (ImGui::Selectable(id_buf, selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    view.navigate_to_event(ev_idx, model.events()[ev_idx], 2.0, 1000.0);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(time_buf);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(dur_buf);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
