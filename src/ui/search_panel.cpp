#include "search_panel.h"
#include "format_time.h"
#include "sort_utils.h"
#include "string_utils.h"
#include "tracing.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <unordered_set>

void SearchPanel::reset() {
    TRACE_FUNCTION_CAT("ui");
    search_buf_[0] = '\0';
    needs_search_ = false;
    unique_by_name_ = true;
    sorted_results_.clear();
    needs_sort_ = false;
    scroll_to_top_ = false;
    name_stats_.clear();
}

std::vector<uint32_t> SearchPanel::filter_unique_by_name(const TraceModel& model,
                                                         const std::vector<uint32_t>& results) {
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> filtered;
    for (uint32_t idx : results) {
        const auto& ev = model.events()[idx];
        if (seen.insert(ev.name_idx).second) {
            filtered.push_back(idx);
        }
    }
    return filtered;
}

void SearchPanel::build_name_stats(const TraceModel& model, const std::vector<uint32_t>& results) {
    name_stats_.clear();
    for (uint32_t idx : results) {
        const auto& ev = model.events()[idx];
        auto& stats = name_stats_[ev.name_idx];
        stats.count++;
        stats.total_dur += ev.dur;
    }
    for (auto& [name_idx, stats] : name_stats_) {
        stats.avg_dur = stats.count > 0 ? stats.total_dur / stats.count : 0.0;
    }
}

void SearchPanel::on_model_changed() {
    reset();
}

void SearchPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_FUNCTION_CAT("ui");
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
        build_name_stats(model, sorted_results_);
        if (unique_by_name_) {
            sorted_results_ = filter_unique_by_name(model, sorted_results_);
        }
        needs_sort_ = true;
    }

    if (ImGui::Checkbox("Unique by name", &unique_by_name_)) {
        // Rebuild sorted_results_ from full results when toggling
        sorted_results_ = view.search_results();
        if (unique_by_name_) {
            sorted_results_ = filter_unique_by_name(model, sorted_results_);
        }
        needs_sort_ = true;
    }

    ImGui::Text("%zu results", sorted_results_.size());

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
        ImGui::BeginTable("SearchResults", 5,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable,
                          ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_None, 0.0f, 3);
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_None, 0.0f, 4);
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
                            case 3: {  // Count
                                auto it_a = name_stats_.find(a.name_idx);
                                auto it_b = name_stats_.find(b.name_idx);
                                uint32_t ca = it_a != name_stats_.end() ? it_a->second.count : 0;
                                uint32_t cb = it_b != name_stats_.end() ? it_b->second.count : 0;
                                cmp = sort_utils::three_way_cmp(ca, cb);
                                break;
                            }
                            case 4: {  // Avg
                                auto it_a = name_stats_.find(a.name_idx);
                                auto it_b = name_stats_.find(b.name_idx);
                                double aa = it_a != name_stats_.end() ? it_a->second.avg_dur : 0.0;
                                double ab = it_b != name_stats_.end() ? it_b->second.avg_dur : 0.0;
                                cmp = sort_utils::three_way_cmp(aa, ab);
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
                auto stats_it = name_stats_.find(ev.name_idx);
                if (stats_it != name_stats_.end()) {
                    ImGui::Text("%u", stats_it->second.count);
                }

                ImGui::TableNextColumn();
                if (stats_it != name_stats_.end()) {
                    char avg_buf[64];
                    format_time(stats_it->second.avg_dur, avg_buf, sizeof(avg_buf));
                    ImGui::TextUnformatted(avg_buf);
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name.c_str());
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
