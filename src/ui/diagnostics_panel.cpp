#include "diagnostics_panel.h"
#include "tracing.h"
#include "format_time.h"
#include "platform/memory.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>

static void format_bytes(size_t bytes, char* buf, size_t buf_size) {
    TRACE_VERBOSE_FUNCTION_CAT("ui");
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(buf, buf_size, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(buf, buf_size, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, buf_size, "%zu B", bytes);
}

void DiagnosticsPanel::render(const TraceModel& model, const ViewState& view) {
    TRACE_FUNCTION_CAT("ui");

    // Update frame timing and memory before Begin() so history stays accurate
    // even when the Diagnostics window is closed (current_rss_mb_ is also read by the toolbar)
    auto now = std::chrono::steady_clock::now();
    size_t rss = get_rss_bytes();
    current_rss_mb_ = rss / (1024.0f * 1024.0f);
    if (!first_frame_) {
        float dt = std::chrono::duration<float>(now - last_frame_).count();
        float fps = (dt > 0.0f) ? 1.0f / dt : 0.0f;
        fps_history_[history_idx_] = fps;
        frame_time_history_[history_idx_] = dt * 1000.0f;
        memory_history_[history_idx_] = current_rss_mb_;
        history_idx_ = (history_idx_ + 1) % HISTORY_SIZE;
    }
    first_frame_ = false;
    last_frame_ = now;

    ImGui::Begin("Diagnostics");

    // FPS section
    if (ImGui::CollapsingHeader("Frame Rate", ImGuiTreeNodeFlags_DefaultOpen)) {
        float current_fps = ImGui::GetIO().Framerate;
        float current_dt = 1000.0f / current_fps;

        ImGui::Text("FPS: %.1f", current_fps);
        ImGui::Text("Frame time: %.2f ms", current_dt);

        // FPS sparkline
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.0f fps", current_fps);
        ImGui::PlotLines("##fps", fps_history_, HISTORY_SIZE, history_idx_, overlay, 0.0f, 200.0f, ImVec2(-1, 50));

        // Frame time sparkline
        snprintf(overlay, sizeof(overlay), "%.1f ms", current_dt);
        ImGui::PlotLines("##frametime", frame_time_history_, HISTORY_SIZE, history_idx_, overlay, 0.0f, 50.0f,
                         ImVec2(-1, 50));
    }

    // Memory section
    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        char rss_str[32];
        format_bytes(rss, rss_str, sizeof(rss_str));
        ImGui::Text("Process RSS: %s", rss_str);

        // Memory sparkline
        float max_mem = 0.0f;
        for (int i = 0; i < HISTORY_SIZE; i++)
            if (memory_history_[i] > max_mem) max_mem = memory_history_[i];
        max_mem = max_mem > 0.0f ? max_mem * 1.2f : 100.0f;
        char mem_overlay[32];
        snprintf(mem_overlay, sizeof(mem_overlay), "%.0f MB", current_rss_mb_);
        ImGui::PlotLines("##memory", memory_history_, HISTORY_SIZE, history_idx_, mem_overlay, 0.0f, max_mem,
                         ImVec2(-1, 50));

        if (!model.events().empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Trace Data");

            size_t events_bytes = model.events().size() * sizeof(TraceEvent);
            char ev_str[32];
            format_bytes(events_bytes, ev_str, sizeof(ev_str));
            ImGui::Text("Events: %s (%zu events)", ev_str, model.events().size());

            char str_str[32];
            format_bytes(model.strings_bytes(), str_str, sizeof(str_str));
            ImGui::Text("String pool: %s (%zu strings)", str_str, model.strings().size());

            char args_str[32];
            format_bytes(model.args_bytes(), args_str, sizeof(args_str));
            ImGui::Text("Args pool: %s (%zu entries)", args_str, model.args().size());

            ImGui::Text("Counter series: %zu (%zu points)", model.counter_series().size(),
                        model.counter_points_count());

            ImGui::Text("Flow groups: %zu", model.flow_groups().size());
        }
    }

    // Trace overview
    if (!model.events().empty() && ImGui::CollapsingHeader("Trace Overview", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Processes: %zu", model.processes().size());
        ImGui::Text("Threads: %d", model.total_threads());
        ImGui::Text("Total events: %zu", model.events().size());

        char dur_buf[64];
        double trace_dur = model.max_ts() - model.min_ts();
        format_time(trace_dur, dur_buf, sizeof(dur_buf));
        ImGui::Text("Trace duration: %s", dur_buf);
    }

    // Viewport / render stats
    if (!model.events().empty() && ImGui::CollapsingHeader("Render Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        double view_dur = view.view_end_ts() - view.view_start_ts();
        char vd_buf[64];
        format_time(view_dur, vd_buf, sizeof(vd_buf));
        ImGui::Text("Viewport span: %s", vd_buf);

        double total_dur = model.max_ts() - model.min_ts();
        if (total_dur > 0.0) {
            ImGui::Text("Zoom level: %.1fx", total_dur / view_dur);
        }

        ImGui::Separator();
        ImGui::Text("Visible slices: %d", stats.visible_slices);
        ImGui::Text("Drawn individually: %d", stats.drawn_slices);
        ImGui::Text("Merged (sub-pixel): %d", stats.merged_slices);
        ImGui::Text("Merge runs drawn: %d", stats.merge_runs);
        ImGui::Text("Labels drawn: %d", stats.labels_drawn);
        ImGui::Text("Instant events: %d", stats.instant_events);
        ImGui::Text("Tracks visible: %d", stats.tracks_visible);

        if (stats.visible_slices > 0 && stats.merged_slices > 0) {
            float merge_pct = 100.0f * stats.merged_slices / stats.visible_slices;
            ImGui::Text("Merge ratio: %.1f%%", merge_pct);
        }
    }

    // ImGui internals
    if (ImGui::CollapsingHeader("ImGui")) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("Vertices: %d", io.MetricsRenderVertices);
        ImGui::Text("Indices: %d", io.MetricsRenderIndices);
        ImGui::Text("Draw calls: %d", io.MetricsRenderWindows);
        ImGui::Text("Active windows: %d", io.MetricsActiveWindows);
    }

    ImGui::End();
}
