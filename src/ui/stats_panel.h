#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <vector>
#include <string>

class StatsPanel {
public:
    void render(const TraceModel& model, ViewState& view);

private:
    struct FuncStats {
        uint32_t name_idx = 0;
        uint32_t count = 0;
        double total_dur = 0.0;
        double min_dur = 0.0;
        double max_dur = 0.0;
        double avg_dur = 0.0;
    };

    std::vector<FuncStats> stats_;
    bool needs_rebuild_ = true;
    uint32_t last_event_count_ = 0;

    void rebuild(const TraceModel& model, const ViewState& view);
};
