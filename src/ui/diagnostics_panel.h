#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <chrono>

struct DiagStats {
    // Per-frame render stats (set by TimelineView)
    int visible_slices = 0;
    int drawn_slices = 0;
    int merged_slices = 0;
    int merge_runs = 0;
    int labels_drawn = 0;
    int tracks_visible = 0;
    int instant_events = 0;
};

class DiagnosticsPanel {
public:
    void render(const TraceModel& model, const ViewState& view);

    // Current RSS in MB, updated each frame (used by toolbar)
    float current_rss_mb() const { return current_rss_mb_; }

    // Updated each frame by TimelineView
    DiagStats stats;

private:
    // FPS history for sparkline
    static constexpr int HISTORY_SIZE = 120;
    float fps_history_[HISTORY_SIZE] = {};
    float frame_time_history_[HISTORY_SIZE] = {};
    float memory_history_[HISTORY_SIZE] = {};  // RSS in MB
    int history_idx_ = 0;

    std::chrono::steady_clock::time_point last_frame_;
    bool first_frame_ = true;

    float current_rss_mb_ = 0.0f;
};
