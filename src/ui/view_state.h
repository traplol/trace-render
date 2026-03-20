#pragma once
#include "model/trace_event.h"
#include "ui/key_bindings.h"
#include "imgui.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

class ViewState {
public:
    // --- Viewport ---
    double view_start_ts() const { return view_start_ts_; }
    double view_end_ts() const { return view_end_ts_; }
    void set_view_start_ts(double ts) { view_start_ts_ = ts; }
    void set_view_end_ts(double ts) { view_end_ts_ = ts; }
    void set_view_range(double start, double end) {
        view_start_ts_ = start;
        view_end_ts_ = end;
        clamp_view_to_bounds();
    }

    // --- Trace time bounds (set once after loading) ---
    void set_trace_bounds(double min_ts, double max_ts) {
        trace_min_ts_ = min_ts;
        trace_max_ts_ = max_ts;
        has_trace_bounds_ = true;
    }

    // --- Selection ---
    int32_t selected_event_idx() const { return selected_event_idx_; }
    void set_selected_event_idx(int32_t idx) { selected_event_idx_ = idx; }
    int32_t pending_scroll_event_idx() const { return pending_scroll_event_idx_; }
    void set_pending_scroll_event_idx(int32_t idx) { pending_scroll_event_idx_ = idx; }

    // --- Range selection ---
    bool has_range_selection() const { return has_range_selection_; }
    bool range_selecting() const { return range_selecting_; }
    void set_range_selecting(bool v) { range_selecting_ = v; }
    double range_start_ts() const { return range_start_ts_; }
    double range_end_ts() const { return range_end_ts_; }

    void set_range_selection(double start, double end) {
        has_range_selection_ = true;
        range_start_ts_ = std::min(start, end);
        range_end_ts_ = std::max(start, end);
    }

    void clear_range_selection() {
        has_range_selection_ = false;
        range_selecting_ = false;
        range_start_ts_ = 0.0;
        range_end_ts_ = 0.0;
    }

    // --- Filtering ---
    const std::unordered_set<uint32_t>& hidden_pids() const { return hidden_pids_; }
    const std::unordered_set<uint32_t>& hidden_tids() const { return hidden_tids_; }
    const std::unordered_set<uint32_t>& hidden_cats() const { return hidden_cats_; }
    void hide_pid(uint32_t pid) { hidden_pids_.insert(pid); }
    void show_pid(uint32_t pid) { hidden_pids_.erase(pid); }
    void hide_tid(uint32_t tid) { hidden_tids_.insert(tid); }
    void show_tid(uint32_t tid) { hidden_tids_.erase(tid); }
    void hide_cat(uint32_t cat) { hidden_cats_.insert(cat); }
    void show_cat(uint32_t cat) { hidden_cats_.erase(cat); }
    void clear_hidden_pids() { hidden_pids_.clear(); }
    void clear_hidden_tids() { hidden_tids_.clear(); }
    void clear_hidden_cats() { hidden_cats_.clear(); }

    // --- Search ---
    const std::string& search_query() const { return search_query_; }
    void set_search_query(const std::string& q) { search_query_ = q; }
    void clear_search_query() { search_query_.clear(); }
    const std::vector<uint32_t>& search_results() const { return search_results_; }
    void set_search_results(std::vector<uint32_t> results) { search_results_ = std::move(results); }
    void add_search_result(uint32_t idx) { search_results_.push_back(idx); }
    void clear_search_results() { search_results_.clear(); }
    int32_t search_current() const { return search_current_; }
    void set_search_current(int32_t idx) { search_current_ = idx; }

    // --- Layout defaults ---
    static constexpr float kDefaultTrackHeight = 54.0f;
    static constexpr float kDefaultTrackPadding = 8.0f;
    static constexpr float kDefaultCounterTrackHeight = 175.0f;
    static constexpr float kDefaultLabelWidth = 221.0f;
    static constexpr float kDefaultRulerHeight = 37.0f;
    static constexpr float kDefaultProcHeaderHeight = 36.0f;
    static constexpr float kDefaultScrollbarScale = 1.3f;

    // --- Layout ---
    float track_height() const { return track_height_; }
    void set_track_height(float h) { track_height_ = h; }
    float track_padding() const { return track_padding_; }
    void set_track_padding(float p) { track_padding_ = p; }
    float counter_track_height() const { return counter_track_height_; }
    void set_counter_track_height(float h) { counter_track_height_ = h; }
    float label_width() const { return label_width_; }
    void set_label_width(float w) { label_width_ = w; }
    float ruler_height() const { return ruler_height_; }
    void set_ruler_height(float h) { ruler_height_ = h; }
    float proc_header_height() const { return proc_header_height_; }
    void set_proc_header_height(float h) { proc_header_height_ = h; }
    float scrollbar_scale() const { return scrollbar_scale_; }
    void set_scrollbar_scale(float s) { scrollbar_scale_ = s; }

    void reset_layout_defaults() {
        track_height_ = kDefaultTrackHeight;
        track_padding_ = kDefaultTrackPadding;
        counter_track_height_ = kDefaultCounterTrackHeight;
        label_width_ = kDefaultLabelWidth;
        ruler_height_ = kDefaultRulerHeight;
        proc_header_height_ = kDefaultProcHeaderHeight;
        scrollbar_scale_ = kDefaultScrollbarScale;
    }

    // --- Rendering defaults ---
    static constexpr bool kDefaultShowFlows = true;
    static constexpr std::array<float, 4> kDefaultSelBorderColor = {0.0f, 0.0f, 0.0f, 1.0f};

    void reset_rendering_defaults() {
        show_flows_ = kDefaultShowFlows;
        sel_border_color_ = kDefaultSelBorderColor;
    }

    // --- Selection border color ---
    const std::array<float, 4>& sel_border_color() const { return sel_border_color_; }
    void set_sel_border_color(const std::array<float, 4>& color) { sel_border_color_ = color; }
    ImU32 sel_border_color_u32() const {
        return IM_COL32((int)(sel_border_color_[0] * 255), (int)(sel_border_color_[1] * 255),
                        (int)(sel_border_color_[2] * 255), (int)(sel_border_color_[3] * 255));
    }

    // --- Key bindings ---
    KeyBindings& key_bindings() { return key_bindings_; }
    const KeyBindings& key_bindings() const { return key_bindings_; }

    // --- Misc ---
    bool show_flows() const { return show_flows_; }
    void set_show_flows(bool v) { show_flows_ = v; }
    bool time_unit_ns() const { return time_unit_ns_; }
    void set_time_unit_ns(bool v) { time_unit_ns_ = v; }

    // --- Coordinate conversion ---
    float time_to_x(double ts, float timeline_left, float timeline_width) const {
        return timeline_left + (float)((ts - view_start_ts_) / (view_end_ts_ - view_start_ts_)) * timeline_width;
    }

    double x_to_time(float x, float timeline_left, float timeline_width) const {
        return view_start_ts_ + (double)(x - timeline_left) / timeline_width * (view_end_ts_ - view_start_ts_);
    }

    void zoom_to_fit(double min_ts, double max_ts) {
        double padding = (max_ts - min_ts) * 0.02;
        view_start_ts_ = min_ts - padding;
        view_end_ts_ = max_ts + padding;
    }

    // Select an event and zoom the viewport to show it.
    void navigate_to_event(int32_t ev_idx, const TraceEvent& ev, double pad_factor = 0.5, double min_pad_us = 100.0) {
        selected_event_idx_ = ev_idx;
        pending_scroll_event_idx_ = ev_idx;
        double pad = std::max(ev.dur * pad_factor, min_pad_us);
        view_start_ts_ = ev.ts - pad;
        view_end_ts_ = ev.end_ts() + pad;
    }

private:
    void clamp_view_to_bounds() {
        if (!has_trace_bounds_) return;
        double range = view_end_ts_ - view_start_ts_;
        double total = trace_max_ts_ - trace_min_ts_;
        // Allow padding up to 2% of total range beyond the bounds
        double pad = total * 0.02;
        double lo = trace_min_ts_ - pad;
        double hi = trace_max_ts_ + pad;
        if (range >= (hi - lo)) {
            // Viewport wider than trace — center it
            double center = (trace_min_ts_ + trace_max_ts_) / 2.0;
            view_start_ts_ = center - range / 2.0;
            view_end_ts_ = center + range / 2.0;
        } else {
            if (view_start_ts_ < lo) {
                view_start_ts_ = lo;
                view_end_ts_ = lo + range;
            }
            if (view_end_ts_ > hi) {
                view_end_ts_ = hi;
                view_start_ts_ = hi - range;
            }
        }
    }

    double view_start_ts_ = 0.0;
    double view_end_ts_ = 1000.0;
    double trace_min_ts_ = 0.0;
    double trace_max_ts_ = 0.0;
    bool has_trace_bounds_ = false;
    int32_t selected_event_idx_ = -1;
    int32_t pending_scroll_event_idx_ = -1;
    bool has_range_selection_ = false;
    bool range_selecting_ = false;
    double range_start_ts_ = 0.0;
    double range_end_ts_ = 0.0;
    std::unordered_set<uint32_t> hidden_pids_;
    std::unordered_set<uint32_t> hidden_tids_;
    std::unordered_set<uint32_t> hidden_cats_;
    std::string search_query_;
    std::vector<uint32_t> search_results_;
    int32_t search_current_ = -1;
    float track_height_ = kDefaultTrackHeight;
    float track_padding_ = kDefaultTrackPadding;
    float counter_track_height_ = kDefaultCounterTrackHeight;
    float label_width_ = kDefaultLabelWidth;
    float ruler_height_ = kDefaultRulerHeight;
    float proc_header_height_ = kDefaultProcHeaderHeight;
    float scrollbar_scale_ = kDefaultScrollbarScale;
    std::array<float, 4> sel_border_color_ = kDefaultSelBorderColor;
    bool show_flows_ = kDefaultShowFlows;
    bool time_unit_ns_ = false;
    KeyBindings key_bindings_;
};
