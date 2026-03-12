#include "diagnostics_panel.h"
#include "format_time.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>

#if defined(__linux__)
#include <unistd.h>
#include <cstdio>
static size_t get_rss_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0;
    long dummy = 0;
    if (fscanf(f, "%ld %ld", &dummy, &pages) != 2) pages = 0;
    fclose(f);
    return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE);
}
#elif defined(__APPLE__)
#include <mach/mach.h>
static size_t get_rss_bytes() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.resident_size;
}
#else
static size_t get_rss_bytes() {
    return 0;
}
#endif

static void format_bytes(size_t bytes, char* buf, size_t buf_size) {
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
    ImGui::Begin("Diagnostics");

    // Update frame timing
    auto now = std::chrono::steady_clock::now();
    if (!first_frame_) {
        float dt = std::chrono::duration<float>(now - last_frame_).count();
        float fps = (dt > 0.0f) ? 1.0f / dt : 0.0f;
        fps_history_[history_idx_] = fps;
        frame_time_history_[history_idx_] = dt * 1000.0f;
        history_idx_ = (history_idx_ + 1) % HISTORY_SIZE;
    }
    first_frame_ = false;
    last_frame_ = now;

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
        size_t rss = get_rss_bytes();
        char rss_str[32];
        format_bytes(rss, rss_str, sizeof(rss_str));
        ImGui::Text("Process RSS: %s", rss_str);

        if (!model.events_.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Trace Data");

            size_t events_bytes = model.events_.size() * sizeof(TraceEvent);
            char ev_str[32];
            format_bytes(events_bytes, ev_str, sizeof(ev_str));
            ImGui::Text("Events: %s (%zu events)", ev_str, model.events_.size());

            size_t strings_bytes = 0;
            for (const auto& s : model.strings_) strings_bytes += s.capacity();
            char str_str[32];
            format_bytes(strings_bytes, str_str, sizeof(str_str));
            ImGui::Text("String pool: %s (%zu strings)", str_str, model.strings_.size());

            size_t args_bytes = 0;
            for (const auto& a : model.args_) args_bytes += a.capacity();
            char args_str[32];
            format_bytes(args_bytes, args_str, sizeof(args_str));
            ImGui::Text("Args pool: %s (%zu entries)", args_str, model.args_.size());

            size_t counter_points = 0;
            for (const auto& cs : model.counter_series_) counter_points += cs.points.size();
            ImGui::Text("Counter series: %zu (%zu points)", model.counter_series_.size(), counter_points);

            ImGui::Text("Flow groups: %zu", model.flow_groups_.size());
        }
    }

    // Trace overview
    if (!model.events_.empty() && ImGui::CollapsingHeader("Trace Overview", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Processes: %zu", model.processes_.size());
        int total_threads = 0;
        for (const auto& proc : model.processes_) total_threads += (int)proc.threads.size();
        ImGui::Text("Threads: %d", total_threads);
        ImGui::Text("Total events: %zu", model.events_.size());

        char dur_buf[64];
        double trace_dur = model.max_ts_ - model.min_ts_;
        format_time(trace_dur, dur_buf, sizeof(dur_buf));
        ImGui::Text("Trace duration: %s", dur_buf);
    }

    // Viewport / render stats
    if (!model.events_.empty() && ImGui::CollapsingHeader("Render Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        double view_dur = view.view_end_ts - view.view_start_ts;
        char vd_buf[64];
        format_time(view_dur, vd_buf, sizeof(vd_buf));
        ImGui::Text("Viewport span: %s", vd_buf);

        double total_dur = model.max_ts_ - model.min_ts_;
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
