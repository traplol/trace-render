#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class DetailPanel {
public:
    void render(const TraceModel& model, ViewState& view);

private:
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
        float pct;             // total_dur as % of parent
        uint32_t longest_idx;  // event_idx of longest instance
    };

    int32_t cached_event_idx_ = -1;
    bool include_all_descendants_ = false;
    bool cached_descendants_flag_ = false;
    bool children_dirty_ = false;
    bool group_by_name_ = false;
    bool cached_group_flag_ = false;
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
};
