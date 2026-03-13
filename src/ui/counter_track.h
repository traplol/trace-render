#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include "imgui.h"
#include <vector>

struct CounterHitResult {
    const CounterSeries* series = nullptr;
    double timestamp = 0.0;
    double value = 0.0;
};

// Find the last counter point at or before the given time (step-function lookup).
// Returns false if time is before the first point or series is empty.
bool counter_lookup_value(const CounterSeries& series, double time, double& out_timestamp, double& out_value);

class CounterTrackRenderer {
public:
    // Renders all counter tracks for a given process below the thread tracks.
    // Returns the total height consumed.
    float render(ImDrawList* dl, ImVec2 area_min, float y_offset, float width, const TraceModel& model, uint32_t pid,
                 const ViewState& view);

    // Render a single counter series
    void render_series(ImDrawList* dl, ImVec2 track_min, ImVec2 track_max, const CounterSeries& series,
                       const ViewState& view, ImU32 color);

    // Hit test: returns true if mouse is over a counter track, fills result
    bool hit_test(float mouse_x, float mouse_y, const ViewState& view, CounterHitResult& result) const;

private:
    struct Layout {
        const CounterSeries* series;
        ImVec2 track_min;
        ImVec2 track_max;
    };
    std::vector<Layout> layouts_;
};
