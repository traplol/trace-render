#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include "ui/range_stats.h"
#include <unordered_set>

class DetailPanel {
public:
    void render(const TraceModel& model, ViewState& view);

private:
    // Range selection cache
    bool range_dirty_ = true;
    double cached_range_start_ = 0.0;
    double cached_range_end_ = 0.0;
    RangeStats range_stats_;

    void render_range_selection(const TraceModel& model, ViewState& view);

    struct ChildInfo {
        uint32_t event_idx;
        uint32_t name_idx;
        double dur;
        float pct;
    };

    struct AggregatedChild {
        uint32_t name_idx;
        uint32_t count;
        double total_dur;
        double avg_dur;
        double min_dur;
        double max_dur;
        float pct;             // total_dur as % of parent
        uint32_t longest_idx;  // event_idx of longest instance
    };

    int32_t cached_event_idx_ = -1;
    bool include_all_descendants_ = false;
    bool cached_descendants_flag_ = false;
    bool children_dirty_ = false;
    bool group_by_name_ = false;
    bool cached_group_flag_ = false;
    int32_t cached_stack_event_idx_ = -1;
    std::vector<uint32_t> cached_call_stack_;
    std::vector<uint32_t> cached_stack_children_;
    std::unordered_set<uint32_t> stack_collapsed_;     // collapsed event indices
    std::unordered_set<uint32_t> stack_has_children_;  // events that have children in the tree
    char filter_buf_[256] = {};
    std::string active_filter_;
    std::vector<ChildInfo> children_;
    std::vector<size_t> filtered_children_;
    std::vector<AggregatedChild> aggregated_;
    std::vector<size_t> filtered_aggregated_;
    double self_time_ = 0.0;
    float self_pct_ = 0.0f;

    void rebuild_children(const TraceModel& model, const TraceEvent& ev);
    void rebuild_aggregated(const TraceModel& model, double parent_dur);
    void rebuild_filter(const TraceModel& model);
    void render_children_table(const TraceModel& model, ViewState& view);
    void render_aggregated_table(const TraceModel& model, ViewState& view);
};
