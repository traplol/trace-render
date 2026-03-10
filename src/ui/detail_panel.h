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

    int32_t cached_event_idx_ = -1;
    bool include_all_descendants_ = false;
    bool cached_descendants_flag_ = false;
    std::vector<ChildInfo> children_;
    double self_time_ = 0.0;
    float self_pct_ = 0.0f;

    void rebuild_children(const TraceModel& model, const TraceEvent& ev);
};
