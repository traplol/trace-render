#include "event_browser.h"
#include "format_time.h"
#include "sort_utils.h"
#include "string_utils.h"
#include "tracing.h"
#include "imgui.h"
#include <algorithm>
#include <unordered_map>

// Returns a color interpolated from blue (cool, 0%) through green/yellow to red (hot, 100%)
static ImVec4 heat_color(float pct) {
    float t = std::min(std::max(pct / 100.0f, 0.0f), 1.0f);
    float r, g, b;
    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0.0f;
        g = s;
        b = 1.0f;
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = 0.0f;
        g = 1.0f;
        b = 1.0f - s;
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = s;
        g = 1.0f;
        b = 0.0f;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 1.0f;
        g = 1.0f - s;
        b = 0.0f;
    }
    return ImVec4(r, g, b, 1.0f);
}

static void render_heat_bar(float pct) {
    ImVec4 col = heat_color(pct);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(pct / 100.0f, ImVec2(-1, 0), "");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetItemRectSize().x);
    ImGui::Text("%.1f%%", pct);
}

EventBrowser::EventBrowser(bool default_group_by_name)
    : group_by_name_(default_group_by_name), default_group_by_name_(default_group_by_name) {}

void EventBrowser::reset() {
    entries_.clear();
    aggregated_.clear();
    filtered_.clear();
    filtered_agg_.clear();
    parent_dur_ = 0.0;
    dirty_ = false;
    group_by_name_ = default_group_by_name_;
    cached_group_ = false;
    filter_buf_[0] = '\0';
    active_filter_.clear();
    scroll_to_top_ = false;
    scroll_agg_to_top_ = false;
}

void EventBrowser::set_entries(std::vector<Entry> entries, double parent_dur, const TraceModel& model) {
    entries_ = std::move(entries);
    parent_dur_ = parent_dur;
    dirty_ = true;
    if (group_by_name_) {
        rebuild_aggregated(model);
    }
    rebuild_filter(model);
}

void EventBrowser::render(const char* id, const TraceModel& model, ViewState& view) {
    ImGui::PushID(id);

    if (cached_group_ != group_by_name_ || dirty_) {
        cached_group_ = group_by_name_;
        if (group_by_name_) {
            rebuild_aggregated(model);
        }
        rebuild_filter(model);
    }

    ImGui::Checkbox("Group by name", &group_by_name_);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "Filter by name...", filter_buf_, sizeof(filter_buf_))) {
        rebuild_filter(model);
    }
    if (filter_buf_[0] != '\0') {
        size_t shown = group_by_name_ ? filtered_agg_.size() : filtered_.size();
        size_t total = group_by_name_ ? aggregated_.size() : entries_.size();
        ImGui::TextDisabled("Showing %zu / %zu", shown, total);
    }

    ImGui::Spacing();

    if (group_by_name_) {
        render_aggregated_table(model, view);
    } else {
        render_table(model, view);
    }

    ImGui::PopID();
}

void EventBrowser::rebuild_aggregated(const TraceModel& model) {
    aggregated_.clear();
    std::unordered_map<uint32_t, size_t> name_to_idx;
    name_to_idx.reserve(entries_.size() / 4);

    for (const auto& e : entries_) {
        auto it = name_to_idx.find(e.name_idx);
        if (it == name_to_idx.end()) {
            name_to_idx[e.name_idx] = aggregated_.size();
            aggregated_.push_back({e.name_idx, 1, e.dur, 0.0, e.dur, e.dur, 0.0f, e.event_idx});
        } else {
            auto& ag = aggregated_[it->second];
            ag.count++;
            ag.total_dur += e.dur;
            if (e.dur < ag.min_dur) ag.min_dur = e.dur;
            if (e.dur > ag.max_dur) ag.max_dur = e.dur;
            if (e.dur > model.events()[ag.longest_idx].dur) {
                ag.longest_idx = e.event_idx;
            }
        }
    }

    for (auto& ag : aggregated_) {
        ag.avg_dur = ag.total_dur / ag.count;
        ag.pct = parent_dur_ > 0 ? (float)(ag.total_dur / parent_dur_ * 100.0) : 0.0f;
    }
}

void EventBrowser::rebuild_filter(const TraceModel& model) {
    TRACE_FUNCTION_CAT("ui");
    active_filter_ = filter_buf_;

    filtered_.clear();
    for (size_t i = 0; i < entries_.size(); i++) {
        if (contains_case_insensitive(model.get_string(entries_[i].name_idx), active_filter_)) {
            filtered_.push_back(i);
        }
    }

    filtered_agg_.clear();
    for (size_t i = 0; i < aggregated_.size(); i++) {
        if (contains_case_insensitive(model.get_string(aggregated_[i].name_idx), active_filter_)) {
            filtered_agg_.push_back(i);
        }
    }
}

void EventBrowser::render_table(const TraceModel& model, ViewState& view) {
    if (ImGui::BeginTable("Table", 3,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
        ImGui::TableSetupColumn(
            "Duration", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 1);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 2);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (dirty_) sort_specs->SpecsDirty = true;
            if (sort_specs->SpecsDirty) {
                bool user_sorted = !dirty_;
                sort_specs->SpecsDirty = false;
                dirty_ = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(filtered_.begin(), filtered_.end(), [&](size_t ai, size_t bi) {
                        const auto& a = entries_[ai];
                        const auto& b = entries_[bi];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0:
                                cmp = model.get_string(a.name_idx).compare(model.get_string(b.name_idx));
                                break;
                            case 1:
                                cmp = sort_utils::three_way_cmp(a.dur, b.dur);
                                break;
                            case 2:
                                cmp = sort_utils::three_way_cmp(a.pct, b.pct);
                                break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                }
                if (user_sorted) scroll_to_top_ = true;
            }
        }

        if (scroll_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top_ = false;
        }

        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& e = entries_[filtered_[i]];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##r%d", i);
                bool is_selected = (view.selected_event_idx() == (int32_t)e.event_idx);
                if (ImGui::Selectable(id_buf, is_selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    view.navigate_to_event(e.event_idx, model.events()[e.event_idx]);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(model.get_string(e.name_idx).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", model.get_string(e.name_idx).c_str());

                ImGui::TableNextColumn();
                format_time(e.dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                render_heat_bar(e.pct);
            }
        }

        ImGui::EndTable();
    }
}

void EventBrowser::render_aggregated_table(const TraceModel& model, ViewState& view) {
    if (ImGui::BeginTable("AggTable", 7,
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                                0.0f, 2);
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_None, 0.0f, 3);
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_None, 0.0f, 4);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_None, 0.0f, 5);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 6);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
            if (dirty_) sort_specs->SpecsDirty = true;
            if (sort_specs->SpecsDirty) {
                bool user_sorted = !dirty_;
                sort_specs->SpecsDirty = false;
                dirty_ = false;
                if (sort_specs->SpecsCount > 0) {
                    const auto& spec = sort_specs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    std::sort(filtered_agg_.begin(), filtered_agg_.end(), [&](size_t ai, size_t bi) {
                        const auto& a = aggregated_[ai];
                        const auto& b = aggregated_[bi];
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0:
                                cmp = model.get_string(a.name_idx).compare(model.get_string(b.name_idx));
                                break;
                            case 1:
                                cmp = sort_utils::three_way_cmp(a.count, b.count);
                                break;
                            case 2:
                                cmp = sort_utils::three_way_cmp(a.total_dur, b.total_dur);
                                break;
                            case 3:
                                cmp = sort_utils::three_way_cmp(a.avg_dur, b.avg_dur);
                                break;
                            case 4:
                                cmp = sort_utils::three_way_cmp(a.min_dur, b.min_dur);
                                break;
                            case 5:
                                cmp = sort_utils::three_way_cmp(a.max_dur, b.max_dur);
                                break;
                            case 6:
                                cmp = sort_utils::three_way_cmp(a.pct, b.pct);
                                break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                }
                if (user_sorted) scroll_agg_to_top_ = true;
            }
        }

        if (scroll_agg_to_top_) {
            ImGui::SetScrollY(0.0f);
            scroll_agg_to_top_ = false;
        }

        char buf[64];
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_agg_.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& ag = aggregated_[filtered_agg_[i]];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "##a%d", i);
                if (ImGui::Selectable(id_buf, false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    view.navigate_to_event(ag.longest_idx, model.events()[ag.longest_idx]);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(model.get_string(ag.name_idx).c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", model.get_string(ag.name_idx).c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%u", ag.count);

                ImGui::TableNextColumn();
                format_time(ag.total_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.avg_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.min_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                format_time(ag.max_dur, buf, sizeof(buf));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextColumn();
                render_heat_bar(ag.pct);
            }
        }

        ImGui::EndTable();
    }
}
