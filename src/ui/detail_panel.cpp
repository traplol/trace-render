#include "detail_panel.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstdio>

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
        case Phase::DurationBegin: return "Duration Begin (B)";
        case Phase::DurationEnd:   return "Duration End (E)";
        case Phase::Complete:      return "Complete (X)";
        case Phase::Instant:       return "Instant (i)";
        case Phase::Counter:       return "Counter (C)";
        case Phase::AsyncBegin:    return "Async Begin (b)";
        case Phase::AsyncEnd:      return "Async End (e)";
        case Phase::AsyncInstant:  return "Async Instant (n)";
        case Phase::FlowStart:    return "Flow Start (s)";
        case Phase::FlowStep:     return "Flow Step (t)";
        case Phase::FlowEnd:      return "Flow End (f)";
        case Phase::Metadata:     return "Metadata (M)";
        default:                  return "Unknown";
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

void DetailPanel::render(const TraceModel& model, const ViewState& view) {
    ImGui::Begin("Details");

    if (view.selected_event_idx < 0 ||
        view.selected_event_idx >= (int32_t)model.events_.size()) {
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

    ImGui::End();
}
