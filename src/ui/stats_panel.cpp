#include "stats_panel.h"
#include "imgui.h"
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cfloat>

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
    std::unordered_map<uint32_t, size_t> name_to_idx;

    for (const auto& ev : model.events_) {
        if (ev.is_end_event || ev.ph == Phase::Metadata || ev.ph == Phase::Counter)
            continue;
        if (ev.dur <= 0) continue;

        // Respect filters
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

void StatsPanel::render(const TraceModel& model, ViewState& view) {
    ImGui::Begin("Statistics");

    if (model.events_.empty()) {
        ImGui::TextDisabled("No trace loaded.");
        ImGui::End();
        return;
    }

    // Rebuild when trace changes
    if (needs_rebuild_ || last_event_count_ != (uint32_t)model.events_.size()) {
        rebuild(model, view);
    }

    if (ImGui::Button("Refresh")) {
        rebuild(model, view);
    }
    ImGui::SameLine();
    ImGui::Text("%zu functions", stats_.size());

    ImGui::Separator();

    if (stats_.empty()) {
        ImGui::TextDisabled("No duration events found.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("StatsTable", 6,
            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable,
            ImVec2(0, 0))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_None,              0.0f, 0);
        ImGui::TableSetupColumn("Count",    ImGuiTableColumnFlags_None,              0.0f, 1);
        ImGui::TableSetupColumn("Total",    ImGuiTableColumnFlags_DefaultSort |
                                            ImGuiTableColumnFlags_PreferSortDescending, 0.0f, 2);
        ImGui::TableSetupColumn("Avg",      ImGuiTableColumnFlags_None,              0.0f, 3);
        ImGui::TableSetupColumn("Min",      ImGuiTableColumnFlags_None,              0.0f, 4);
        ImGui::TableSetupColumn("Max",      ImGuiTableColumnFlags_None,              0.0f, 5);
        ImGui::TableHeadersRow();

        // Sort
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
                                case 0: { // Name
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

    ImGui::End();
}
