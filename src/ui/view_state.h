#pragma once
#include "model/trace_event.h"
#include "imgui.h"
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

    // Range selection (drag on ruler)
    bool has_range_selection = false;
    double range_start_ts = 0.0;
    double range_end_ts = 0.0;

    void set_range_selection(double start, double end) {
        has_range_selection = true;
        range_start_ts = std::min(start, end);
        range_end_ts = std::max(start, end);
    }

    void clear_range_selection() {
        has_range_selection = false;
        range_start_ts = 0.0;
        range_end_ts = 0.0;
    }

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

    // Selection border color (RGBA)
    float sel_border_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ImU32 sel_border_color_u32() const {
        return IM_COL32((int)(sel_border_color[0] * 255), (int)(sel_border_color[1] * 255),
                        (int)(sel_border_color[2] * 255), (int)(sel_border_color[3] * 255));
    }

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
