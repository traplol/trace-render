#include "filter_panel.h"
#include "tracing.h"
#include "imgui.h"

void FilterPanel::render(const TraceModel& model, ViewState& view) {
    TRACE_SCOPE_CAT("Filters", "ui");
    ImGui::Begin("Filters");

    // Process / Thread tree
    if (ImGui::CollapsingHeader("Processes & Threads", ImGuiTreeNodeFlags_DefaultOpen)) {
        TRACE_SCOPE_ARGS("Processes & Threads", "ui", "num_processes", (int)model.processes_.size());
        for (const auto& proc : model.processes_) {
            TRACE_SCOPE_ARGS("Process", "ui", "pid", (int)proc.pid, "name", proc.name.c_str(), "num_threads",
                             (int)proc.threads.size());
            bool proc_visible = !view.hidden_pids.count(proc.pid);
            if (ImGui::Checkbox(("##proc_" + std::to_string(proc.pid)).c_str(), &proc_visible)) {
                if (proc_visible)
                    view.hidden_pids.erase(proc.pid);
                else
                    view.hidden_pids.insert(proc.pid);
            }
            ImGui::SameLine();

            if (ImGui::TreeNode(proc.name.c_str())) {
                TRACE_SCOPE_ARGS("ThreadList", "ui", "pid", (int)proc.pid, "num_threads", (int)proc.threads.size());
                for (const auto& thread : proc.threads) {
                    bool thread_visible = !view.hidden_tids.count(thread.tid);
                    if (ImGui::Checkbox(("##thread_" + std::to_string(thread.tid)).c_str(), &thread_visible)) {
                        if (thread_visible)
                            view.hidden_tids.erase(thread.tid);
                        else
                            view.hidden_tids.insert(thread.tid);
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
        TRACE_SCOPE_ARGS("Categories", "ui", "num_categories", (int)model.categories_.size());

        if (ImGui::Button("All")) {
            view.hidden_cats.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            for (uint32_t c : model.categories_) view.hidden_cats.insert(c);
        }

        {
            for (uint32_t cat_idx : model.categories_) {
                const std::string& cat_name = model.get_string(cat_idx);
                bool visible = !view.hidden_cats.count(cat_idx);
                if (ImGui::Checkbox(cat_name.empty() ? "(empty)" : cat_name.c_str(), &visible)) {
                    if (visible)
                        view.hidden_cats.erase(cat_idx);
                    else
                        view.hidden_cats.insert(cat_idx);
                }
            }
        }
    }

    ImGui::End();
}
