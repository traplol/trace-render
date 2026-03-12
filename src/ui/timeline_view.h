#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include "ui/counter_track.h"
#include "ui/flow_renderer.h"
#include "ui/diagnostics_panel.h"
#include "imgui.h"

class TimelineView {
public:
    void render(const TraceModel& model, ViewState& view);

    // Updated each frame, read by DiagnosticsPanel
    DiagStats diag_stats;

private:
    float scroll_y_ = 0.0f;
    float total_content_height_ = 0.0f;
    std::vector<uint32_t> visible_events_;

    // Go-to-time popup
    bool show_goto_ = false;
    char goto_buf_[128] = {};

    CounterTrackRenderer counter_renderer_;
    FlowRenderer flow_renderer_;

    void render_time_ruler(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const ViewState& view);
    void render_tracks(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const TraceModel& model, ViewState& view);
    int32_t hit_test(float click_x, float click_y, ImVec2 area_min, ImVec2 area_max, const TraceModel& model,
                     const ViewState& view);

    struct TrackLayout {
        uint32_t pid;
        uint32_t tid;
        float y_start;
        float height;
        int proc_idx;
        int thread_idx;
    };
    std::vector<TrackLayout> track_layouts_;
};
