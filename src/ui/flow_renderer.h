#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include "imgui.h"
#include <unordered_map>

class FlowRenderer {
public:
    struct TrackPos {
        float y_start;
        float height;
    };

    // Call this to set up track position lookup (pid:tid -> y position)
    void set_track_positions(const std::unordered_map<uint64_t, TrackPos>& positions) { track_positions_ = positions; }

    void render(ImDrawList* dl, const TraceModel& model, const ViewState& view, ImVec2 area_min, ImVec2 area_max,
                float label_width);

    static uint64_t make_key(uint32_t pid, uint32_t tid) { return ((uint64_t)pid << 32) | tid; }

private:
    std::unordered_map<uint64_t, TrackPos> track_positions_;
};
