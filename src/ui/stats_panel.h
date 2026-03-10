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

    // Instance browser for selected function
    int32_t selected_func_ = -1;       // index into stats_
    std::vector<uint32_t> instances_;   // event indices for selected function
    int32_t instance_cursor_ = -1;     // current instance

    void rebuild(const TraceModel& model, const ViewState& view);
    void select_function(int32_t func_idx, const TraceModel& model, const ViewState& view);
    void navigate_to_instance(int32_t idx, const TraceModel& model, ViewState& view);
};
