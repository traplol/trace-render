#include "search_panel.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

static bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return it != haystack.end();
}

static void format_time_search(double us, char* buf, size_t buf_size) {
    double abs_us = std::abs(us);
    if (abs_us < 1.0)
        snprintf(buf, buf_size, "%.1f ns", us * 1000.0);
    else if (abs_us < 1000.0)
        snprintf(buf, buf_size, "%.3f us", us);
    else if (abs_us < 1000000.0)
        snprintf(buf, buf_size, "%.3f ms", us / 1000.0);
    else
        snprintf(buf, buf_size, "%.6f s", us / 1000000.0);
}

void SearchPanel::render(const TraceModel& model, ViewState& view) {
    ImGui::Begin("Search");

    // Focus on Ctrl+F
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::SetNextItemWidth(-60);
    if (ImGui::InputText("##search", search_buf_, sizeof(search_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        needs_search_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Find")) {
        needs_search_ = true;
    }

    if (needs_search_) {
        needs_search_ = false;
        view.search_query = search_buf_;
        view.search_results.clear();
        view.search_current = -1;

        if (!view.search_query.empty()) {
            for (uint32_t i = 0; i < model.events_.size(); i++) {
                const auto& ev = model.events_[i];
                if (ev.is_end_event || ev.ph == Phase::Metadata) continue;
                const std::string& name = model.get_string(ev.name_idx);
                const std::string& cat = model.get_string(ev.cat_idx);
                if (contains_case_insensitive(name, view.search_query) ||
                    contains_case_insensitive(cat, view.search_query)) {
                    view.search_results.push_back(i);
                }
            }
        }
    }

    ImGui::Text("%zu results", view.search_results.size());

    // Navigation
    bool navigate = false;
    if (!view.search_results.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("<") && view.search_current > 0) {
            view.search_current--;
            navigate = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(">")) {
            view.search_current++;
            if (view.search_current >= (int32_t)view.search_results.size())
                view.search_current = (int32_t)view.search_results.size() - 1;
            navigate = true;
        }

        // Only navigate when user explicitly clicks prev/next
        if (navigate && view.search_current >= 0 && view.search_current < (int32_t)view.search_results.size()) {
            uint32_t ev_idx = view.search_results[view.search_current];
            view.selected_event_idx = ev_idx;
            const auto& ev = model.events_[ev_idx];
            double pad = std::max((double)ev.dur * 2.0, 1000.0);
            view.view_start_ts = ev.ts - pad;
            view.view_end_ts = ev.end_ts() + pad;
        }
    }

    ImGui::Separator();

    // Results list
    if (ImGui::BeginChild("results", ImVec2(0, 0), ImGuiChildFlags_None)) {
        ImGuiListClipper clipper;
        clipper.Begin((int)view.search_results.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                uint32_t ev_idx = view.search_results[i];
                const auto& ev = model.events_[ev_idx];
                const std::string& name = model.get_string(ev.name_idx);

                char label[512];
                char time_buf[64];
                format_time_search((double)ev.ts, time_buf, sizeof(time_buf));

                snprintf(label, sizeof(label), "%s [%s]##%d",
                         name.c_str(), time_buf, i);

                bool selected = (view.search_current == i);
                if (ImGui::Selectable(label, selected)) {
                    view.search_current = i;
                    view.selected_event_idx = ev_idx;
                    double pad = std::max((double)ev.dur * 2.0, 1000.0);
                    view.view_start_ts = ev.ts - pad;
                    view.view_end_ts = ev.end_ts() + pad;
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
