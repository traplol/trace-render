#pragma once
#include "model/trace_event.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

struct ViewState {
    // Viewport in trace time (microseconds)
    double view_start_ts = 0.0;
    double view_end_ts = 1000.0;

    // Selection
    int32_t selected_event_idx = -1;
    int32_t pending_scroll_event_idx = -1;  // set by navigate_to_event, consumed by TimelineView

    // Filtering
    std::unordered_set<uint32_t> hidden_pids;
    std::unordered_set<uint32_t> hidden_tids;
    std::unordered_set<uint32_t> hidden_cats;

    // Search
    std::string search_query;
    std::vector<uint32_t> search_results;
    int32_t search_current = -1;

    // Layout
    float track_height = 60.0f;
    float track_padding = 12.0f;
    float counter_track_height = 180.0f;
    float label_width = 600.0f;
    float ruler_height = 30.0f;
    float proc_header_height = 22.0f;
    float scrollbar_scale = 1.0f;

    // Show flow arrows
    bool show_flows = true;

    // Time unit: false = microseconds (default Chrome), true = nanoseconds
    bool time_unit_ns = false;

    float time_to_x(double ts, float timeline_left, float timeline_width) const {
        return timeline_left + (float)((ts - view_start_ts) / (view_end_ts - view_start_ts)) * timeline_width;
    }

    double x_to_time(float x, float timeline_left, float timeline_width) const {
        return view_start_ts + (double)(x - timeline_left) / timeline_width * (view_end_ts - view_start_ts);
    }

    void zoom_to_fit(double min_ts, double max_ts) {
        double padding = (max_ts - min_ts) * 0.02;
        view_start_ts = min_ts - padding;
        view_end_ts = max_ts + padding;
    }

    // Select an event and zoom the viewport to show it.
    // pad_factor controls how much surrounding context is shown (0.5 = tight, 2.0 = wide).
    // min_pad_us is the minimum padding in microseconds (prevents over-zoom on tiny events).
    void navigate_to_event(int32_t ev_idx, const TraceEvent& ev, double pad_factor = 0.5, double min_pad_us = 100.0) {
        selected_event_idx = ev_idx;
        pending_scroll_event_idx = ev_idx;
        double pad = std::max(ev.dur * pad_factor, min_pad_us);
        view_start_ts = ev.ts - pad;
        view_end_ts = ev.end_ts() + pad;
    }
};
