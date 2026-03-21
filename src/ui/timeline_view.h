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

    // Go-to-time popup
    bool show_goto_ = false;
    char goto_buf_[128] = {};

    // Ruler drag for range selection
    bool ruler_dragging_ = false;
    double ruler_drag_start_ts_ = 0.0;

    CounterTrackRenderer counter_renderer_;
    FlowRenderer flow_renderer_;

    void render_time_ruler(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const ViewState& view);
    void render_tracks(ImDrawList* dl, ImVec2 area_min, ImVec2 area_max, const TraceModel& model, ViewState& view);
    int32_t hit_test(float click_x, float click_y, ImVec2 area_min, ImVec2 area_max, const TraceModel& model,
                     const ViewState& view);

public:
    // Select the best (shortest-duration) event from candidates at the given depth/time.
    // Skips end events and events whose category is hidden. Exposed for testing.
    static int32_t select_best_candidate(const std::vector<uint32_t>& candidates, const std::vector<TraceEvent>& events,
                                         const std::unordered_set<uint32_t>& hidden_cats, int clicked_depth,
                                         double click_time, double tolerance);

private:
    struct TrackLayout {
        uint32_t pid;
        uint32_t tid;
        float y_start;
        float height;
        int proc_idx;
        int thread_idx;
    };
    std::vector<TrackLayout> track_layouts_;

    // Deferred selection border (drawn on top of all tracks)
    bool sel_rect_valid_ = false;
    ImVec2 sel_rect_min_;
    ImVec2 sel_rect_max_;
};
