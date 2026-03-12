#include "detail_panel.h"
#include "tracing.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <cmath>
#include <unordered_map>

static void format_time_detail(double us, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);
    if (abs_us < 1.0) {
        snprintf(buf, buf_size, "%.3f ns", us * 1000.0);
    } else if (abs_us < 1000.0) {
        snprintf(buf, buf_size, "%.3f us", us);
    } else if (abs_us < 1000000.0) {
        snprintf(buf, buf_size, "%.3f ms (%.0f us)", us / 1000.0, us);
    } else {
        snprintf(buf, buf_size, "%.3f s (%.0f us)", us / 1000000.0, us);
    }
}

static const char* phase_name(Phase ph) {
    switch (ph) {
        case Phase::DurationBegin:
            return "Duration Begin (B)";
        case Phase::DurationEnd:
            return "Duration End (E)";
        case Phase::Complete:
            return "Complete (X)";
        case Phase::Instant:
            return "Instant (i)";
        case Phase::Counter:
            return "Counter (C)";
        case Phase::AsyncBegin:
            return "Async Begin (b)";
        case Phase::AsyncEnd:
            return "Async End (e)";
        case Phase::AsyncInstant:
            return "Async Instant (n)";
        case Phase::FlowStart:
            return "Flow Start (s)";
        case Phase::FlowStep:
            return "Flow Step (t)";
        case Phase::FlowEnd:
            return "Flow End (f)";
        case Phase::Metadata:
            return "Metadata (M)";
        default:
            return "Unknown";
    }
}

static void render_json_value(const nlohmann::json& j, int depth = 0) {
    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_object() || val.is_array()) {
                if (ImGui::TreeNode(key.c_str())) {
                    render_json_value(val, depth + 1);
                    ImGui::TreePop();
                }
            } else {
                ImGui::Text("%s: %s", key.c_str(), val.dump().c_str());
            }
        }
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); i++) {
            char label[32];
            snprintf(label, sizeof(label), "[%zu]", i);
            if (j[i].is_object() || j[i].is_array()) {
                if (ImGui::TreeNode(label)) {
                    render_json_value(j[i], depth + 1);
                    ImGui::TreePop();
                }
            } else {
                ImGui::Text("%s: %s", label, j[i].dump().c_str());
            }
        }
    } else {
        ImGui::Text("%s", j.dump().c_str());
    }
}

void DetailPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Details", "ui");
    ImGui::Begin("Details");

    if (view.selected_event_idx < 0 || view.selected_event_idx >= (int32_t)model.events_.size()) {
        ImGui::TextDisabled("Click a slice in the timeline to see details.");
        ImGui::End();
        return;
    }

    const auto& ev = model.events_[view.selected_event_idx];

    ImGui::Text("Name: %s", model.get_string(ev.name_idx).c_str());
    ImGui::Separator();

    ImGui::Text("Category: %s", model.get_string(ev.cat_idx).c_str());
    ImGui::Text("Phase: %s", phase_name(ev.ph));

    char time_buf[128];
    format_time_detail((double)ev.ts, time_buf, sizeof(time_buf));
    ImGui::Text("Timestamp: %s", time_buf);

    if (ev.dur > 0) {
        format_time_detail((double)ev.dur, time_buf, sizeof(time_buf));
        ImGui::Text("Duration: %s", time_buf);
    }

    ImGui::Text("Process: %u", ev.pid);
    // Find process name
    for (const auto& proc : model.processes_) {
        if (proc.pid == ev.pid) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", proc.name.c_str());
            break;
        }
    }

    ImGui::Text("Thread: %u", ev.tid);
    for (const auto& proc : model.processes_) {
        if (proc.pid == ev.pid) {
            for (const auto& t : proc.threads) {
                if (t.tid == ev.tid) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", t.name.c_str());
                    break;
                }
            }
            break;
        }
    }

    if (ev.id != 0) {
        ImGui::Text("ID: 0x%llx", (unsigned long long)ev.id);
    }

    ImGui::Text("Depth: %d", ev.depth);

    // Parent button — find the enclosing event at depth-1 on the same thread
    if (ev.depth > 0) {
        for (const auto& proc : model.processes_) {
            if (proc.pid != ev.pid) continue;
            for (const auto& thread : proc.threads) {
                if (thread.tid != ev.tid) continue;
                int32_t parent_idx = -1;
                for (uint32_t idx : thread.event_indices) {
                    const auto& candidate = model.events_[idx];
                    if (candidate.depth == ev.depth - 1 && candidate.ts <= ev.ts && candidate.end_ts() >= ev.end_ts()) {
                        parent_idx = (int32_t)idx;
                        break;
                    }
                }
                if (parent_idx >= 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Parent")) {
                        view.selected_event_idx = parent_idx;
                        const auto& parent = model.events_[parent_idx];
                        double pad = std::max(parent.dur * 0.5, 100.0);
                        view.view_start_ts = parent.ts - pad;
                        view.view_end_ts = parent.end_ts() + pad;
                    }
                }
                goto done_parent;
            }
        }
    done_parent:;
    }

    // Args
    if (ev.args_idx != UINT32_MAX && ev.args_idx < model.args_.size()) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Arguments", ImGuiTreeNodeFlags_DefaultOpen)) {
            try {
                auto args = nlohmann::json::parse(model.args_[ev.args_idx]);
                render_json_value(args);
            } catch (...) {
                ImGui::TextDisabled("(could not parse args)");
                ImGui::TextWrapped("%s", model.args_[ev.args_idx].c_str());
            }
        }
    }

    // Children & time dominance (re-fetch ev in case Parent button changed selection)
    const auto& current_ev = model.events_[view.selected_event_idx];
    if (current_ev.dur > 0) {
        // Rebuild children cache when selection or descendants flag changes
        if (cached_event_idx_ != view.selected_event_idx || cached_descendants_flag_ != include_all_descendants_) {
            cached_event_idx_ = view.selected_event_idx;
            cached_descendants_flag_ = include_all_descendants_;
            rebuild_children(model, current_ev);
            children_dirty_ = true;
        }

        // Rebuild aggregated view when grouping flag changes or children changed
        if (cached_group_flag_ != group_by_name_ || children_dirty_) {
            cached_group_flag_ = group_by_name_;
            if (group_by_name_) {
                rebuild_aggregated(model, current_ev.dur);
            }
        }

        if (!children_.empty()) {
            ImGui::Separator();
            char header_buf[128];
            if (group_by_name_) {
                snprintf(header_buf, sizeof(header_buf), "Children (%zu events, %zu unique)", children_.size(),
                         aggregated_.size());
            } else {
                snprintf(header_buf, sizeof(header_buf), "Children (%zu)", children_.size());
            }
            if (ImGui::CollapsingHeader(header_buf, ImGuiTreeNodeFlags_DefaultOpen)) {
                char self_buf[64];
                format_time_detail(self_time_, self_buf, sizeof(self_buf));
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Self: %s (%.1f%%)", self_buf, self_pct_);
                ImGui::Checkbox("Include all descendants", &include_all_descendants_);
                ImGui::SameLine();
                ImGui::Checkbox("Group by name", &group_by_name_);

                ImGui::Spacing();

                if (group_by_name_) {
                    // --- Aggregated table ---
                    if (ImGui::BeginTable("AggChildrenTable", 5,
                                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
                        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_None, 0.0f, 1);
                        ImGui::TableSetupColumn(
                            "Total", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                            0.0f, 2);
                        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_None, 0.0f, 3);
                        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 4);
                        ImGui::TableHeadersRow();

                        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                            if (children_dirty_) {
                                sort_specs->SpecsDirty = true;
                                children_dirty_ = false;
                            }
                            if (sort_specs->SpecsDirty) {
                                sort_specs->SpecsDirty = false;
                                if (sort_specs->SpecsCount > 0) {
                                    const auto& spec = sort_specs->Specs[0];
                                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                                    std::sort(aggregated_.begin(), aggregated_.end(),
                                              [&](const AggregatedChild& a, const AggregatedChild& b) {
                                                  int cmp = 0;
                                                  switch (spec.ColumnUserID) {
                                                      case 0: {
                                                          const auto& na = model.get_string(a.name_idx);
                                                          const auto& nb = model.get_string(b.name_idx);
                                                          cmp = na.compare(nb);
                                                          break;
                                                      }
                                                      case 1:
                                                          cmp = (a.count < b.count) ? -1 : (a.count > b.count) ? 1 : 0;
                                                          break;
                                                      case 2:
                                                          cmp = (a.total_dur < b.total_dur)   ? -1
                                                                : (a.total_dur > b.total_dur) ? 1
                                                                                              : 0;
                                                          break;
                                                      case 3:
                                                          cmp = (a.avg_dur < b.avg_dur)   ? -1
                                                                : (a.avg_dur > b.avg_dur) ? 1
                                                                                          : 0;
                                                          break;
                                                      case 4:
                                                          cmp = (a.pct < b.pct) ? -1 : (a.pct > b.pct) ? 1 : 0;
                                                          break;
                                                  }
                                                  return asc ? (cmp < 0) : (cmp > 0);
                                              });
                                }
                            }
                        }

                        char buf[64];
                        ImGuiListClipper clipper;
                        clipper.Begin((int)aggregated_.size());
                        while (clipper.Step()) {
                            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                                const auto& ag = aggregated_[i];
                                ImGui::TableNextRow();

                                ImGui::TableNextColumn();
                                char id_buf[32];
                                snprintf(id_buf, sizeof(id_buf), "##ag%d", i);
                                if (ImGui::Selectable(
                                        id_buf, false,
                                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                                    view.selected_event_idx = ag.longest_idx;
                                    const auto& child_ev = model.events_[ag.longest_idx];
                                    double pad = std::max(child_ev.dur * 0.5, 100.0);
                                    view.view_start_ts = child_ev.ts - pad;
                                    view.view_end_ts = child_ev.end_ts() + pad;
                                }
                                ImGui::SameLine();
                                ImGui::TextUnformatted(model.get_string(ag.name_idx).c_str());

                                ImGui::TableNextColumn();
                                ImGui::Text("%u", ag.count);

                                ImGui::TableNextColumn();
                                format_time_detail(ag.total_dur, buf, sizeof(buf));
                                ImGui::TextUnformatted(buf);

                                ImGui::TableNextColumn();
                                format_time_detail(ag.avg_dur, buf, sizeof(buf));
                                ImGui::TextUnformatted(buf);

                                ImGui::TableNextColumn();
                                ImGui::ProgressBar(ag.pct / 100.0f, ImVec2(-1, 0), "");
                                ImGui::SameLine(0, 0);
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetItemRectSize().x);
                                ImGui::Text("%.1f%%", ag.pct);
                            }
                        }

                        ImGui::EndTable();
                    }
                } else {
                    // --- Individual children table (with clipper) ---
                    if (ImGui::BeginTable("ChildrenTable", 3,
                                          ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 0.0f, 0);
                        ImGui::TableSetupColumn(
                            "Duration", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
                            0.0f, 1);
                        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_None, 0.0f, 2);
                        ImGui::TableHeadersRow();

                        if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
                            if (children_dirty_) {
                                sort_specs->SpecsDirty = true;
                                children_dirty_ = false;
                            }
                            if (sort_specs->SpecsDirty) {
                                sort_specs->SpecsDirty = false;
                                if (sort_specs->SpecsCount > 0) {
                                    const auto& spec = sort_specs->Specs[0];
                                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                                    std::sort(children_.begin(), children_.end(),
                                              [&](const ChildInfo& a, const ChildInfo& b) {
                                                  int cmp = 0;
                                                  switch (spec.ColumnUserID) {
                                                      case 0: {
                                                          const auto& na = model.get_string(a.name_idx);
                                                          const auto& nb = model.get_string(b.name_idx);
                                                          cmp = na.compare(nb);
                                                          break;
                                                      }
                                                      case 1:
                                                          cmp = (a.dur < b.dur) ? -1 : (a.dur > b.dur) ? 1 : 0;
                                                          break;
                                                      case 2:
                                                          cmp = (a.pct < b.pct) ? -1 : (a.pct > b.pct) ? 1 : 0;
                                                          break;
                                                  }
                                                  return asc ? (cmp < 0) : (cmp > 0);
                                              });
                                }
                            }
                        }

                        char buf[64];
                        ImGuiListClipper clipper;
                        clipper.Begin((int)children_.size());
                        while (clipper.Step()) {
                            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                                const auto& c = children_[i];
                                ImGui::TableNextRow();

                                ImGui::TableNextColumn();
                                char id_buf[32];
                                snprintf(id_buf, sizeof(id_buf), "##c%d", i);
                                bool is_selected = (view.selected_event_idx == (int32_t)c.event_idx);
                                if (ImGui::Selectable(
                                        id_buf, is_selected,
                                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                                    view.selected_event_idx = c.event_idx;
                                    const auto& child_ev = model.events_[c.event_idx];
                                    double pad = std::max(child_ev.dur * 0.5, 100.0);
                                    view.view_start_ts = child_ev.ts - pad;
                                    view.view_end_ts = child_ev.end_ts() + pad;
                                }
                                ImGui::SameLine();
                                ImGui::TextUnformatted(model.get_string(c.name_idx).c_str());

                                ImGui::TableNextColumn();
                                format_time_detail(c.dur, buf, sizeof(buf));
                                ImGui::TextUnformatted(buf);

                                ImGui::TableNextColumn();
                                ImGui::ProgressBar(c.pct / 100.0f, ImVec2(-1, 0), "");
                                ImGui::SameLine(0, 0);
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() - ImGui::GetItemRectSize().x);
                                ImGui::Text("%.1f%%", c.pct);
                            }
                        }

                        ImGui::EndTable();
                    }
                }
            }
        }
    }

    ImGui::End();
}

void DetailPanel::rebuild_children(const TraceModel& model, const TraceEvent& ev) {
    children_.clear();
    double immediate_children_total = 0.0;

    for (const auto& proc : model.processes_) {
        if (proc.pid != ev.pid) continue;
        for (const auto& thread : proc.threads) {
            if (thread.tid != ev.tid) continue;
            for (uint32_t idx : thread.event_indices) {
                const auto& child = model.events_[idx];
                if (child.depth <= ev.depth) {
                    if (child.ts > ev.end_ts()) break;
                    continue;
                }
                if (child.ts < ev.ts || child.end_ts() > ev.end_ts()) continue;
                if (child.dur <= 0) continue;

                if (child.depth == ev.depth + 1) {
                    immediate_children_total += child.dur;
                }

                if (include_all_descendants_ || child.depth == ev.depth + 1) {
                    float pct = (float)(child.dur / ev.dur * 100.0);
                    children_.push_back({idx, child.name_idx, child.dur, pct});
                }

                if (child.ts > ev.end_ts()) break;
            }
            break;
        }
        break;
    }

    self_time_ = ev.dur - immediate_children_total;
    self_pct_ = (float)(self_time_ / ev.dur * 100.0);
}

void DetailPanel::rebuild_aggregated(const TraceModel& model, double parent_dur) {
    aggregated_.clear();

    // Group by name_idx using a map to accumulator index
    std::unordered_map<uint32_t, size_t> name_to_idx;
    name_to_idx.reserve(children_.size() / 4);

    for (const auto& c : children_) {
        auto it = name_to_idx.find(c.name_idx);
        if (it == name_to_idx.end()) {
            name_to_idx[c.name_idx] = aggregated_.size();
            aggregated_.push_back({c.name_idx, 1, c.dur, 0.0, 0.0f, c.event_idx});
        } else {
            auto& ag = aggregated_[it->second];
            ag.count++;
            ag.total_dur += c.dur;
            if (c.dur > model.events_[ag.longest_idx].dur) {
                ag.longest_idx = c.event_idx;
            }
        }
    }

    for (auto& ag : aggregated_) {
        ag.avg_dur = ag.total_dur / ag.count;
        ag.pct = (float)(ag.total_dur / parent_dur * 100.0);
    }
}
