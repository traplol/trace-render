#include "filter_panel.h"
#include "tracing.h"
#include "imgui.h"

void FilterPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Filters", "ui");
    ImGui::Begin("Filters");

    // Process / Thread tree
    if (ImGui::CollapsingHeader("Processes & Threads", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& proc : model.processes()) {
            bool proc_visible = !view.hidden_pids().count(proc.pid);
            if (ImGui::Checkbox(("##proc_" + std::to_string(proc.pid)).c_str(), &proc_visible)) {
                if (proc_visible)
                    view.show_pid(proc.pid);
                else
                    view.hide_pid(proc.pid);
            }
            ImGui::SameLine();

            if (ImGui::TreeNode(proc.name.c_str())) {
                for (const auto& thread : proc.threads) {
                    bool thread_visible = !view.hidden_tids().count(thread.tid);
                    if (ImGui::Checkbox(("##thread_" + std::to_string(thread.tid)).c_str(), &thread_visible)) {
                        if (thread_visible)
                            view.show_tid(thread.tid);
                        else
                            view.hide_tid(thread.tid);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s (%u events)", thread.name.c_str(), (unsigned)thread.event_indices.size());
                }
                ImGui::TreePop();
            }
        }
    }

    // Category filter
    if (ImGui::CollapsingHeader("Categories", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("All")) {
            view.clear_hidden_cats();
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            for (uint32_t c : model.categories()) view.hide_cat(c);
        }

        for (uint32_t cat_idx : model.categories()) {
            const std::string& cat_name = model.get_string(cat_idx);
            bool visible = !view.hidden_cats().count(cat_idx);
            if (ImGui::Checkbox(cat_name.empty() ? "(empty)" : cat_name.c_str(), &visible)) {
                if (visible)
                    view.show_cat(cat_idx);
                else
                    view.hide_cat(cat_idx);
            }
        }
    }

    ImGui::End();
}
