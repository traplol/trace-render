#pragma once
#include "model/trace_model.h"
#include "ui/event_browser.h"
#include "ui/view_state.h"
#include "ui/range_stats.h"
#include <unordered_set>

class DetailPanel {
public:
    void render(const TraceModel& model, ViewState& view);
    void on_model_changed();

private:
    // NOTE: update reset() when adding cached fields
    void reset();
    // Range selection cache
    double cached_range_start_ = 0.0;
    double cached_range_end_ = 0.0;
    RangeStats range_stats_;

    void render_range_selection(const TraceModel& model, ViewState& view);

    // Children tab
    int32_t cached_event_idx_ = -1;
    bool include_all_descendants_ = false;
    bool cached_descendants_flag_ = false;
    double self_time_ = 0.0;
    float self_pct_ = 0.0f;
    EventBrowser children_browser_;

    void rebuild_children(const TraceModel& model, const TraceEvent& ev);

    // Siblings tab
    int32_t cached_siblings_event_idx_ = -1;
    EventBrowser siblings_browser_{/*default_group_by_name=*/true};

    void rebuild_siblings(const TraceModel& model, const TraceEvent& ev, uint32_t event_idx);

    // Call stack tab
    int32_t cached_stack_event_idx_ = -1;
    std::vector<uint32_t> cached_call_stack_;
    std::vector<uint32_t> cached_stack_children_;
    std::unordered_set<uint32_t> stack_collapsed_;
    std::unordered_set<uint32_t> stack_has_children_;
};
