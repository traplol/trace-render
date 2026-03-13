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

class CounterTrackRenderer {
public:
    // Renders all counter tracks for a given process below the thread tracks.
    // Returns the total height consumed.
    float render(ImDrawList* dl, ImVec2 area_min, float y_offset, float width, const TraceModel& model, uint32_t pid,
                 const ViewState& view);

    // Render a single counter series
    void render_series(ImDrawList* dl, ImVec2 track_min, ImVec2 track_max, const CounterSeries& series,
                       const ViewState& view, ImU32 color);

    // Clear stored layouts (call before render_tracks loop)
    void clear_layouts() { layouts_.clear(); }

    // Hit test: returns true if mouse is over a counter track, fills result
    bool hit_test(float mouse_x, float mouse_y, float track_left, float track_width, const ViewState& view,
                  CounterHitResult& result) const;

private:
    struct Layout {
        const CounterSeries* series;
        ImVec2 track_min;
        ImVec2 track_max;
    };
    std::vector<Layout> layouts_;
};
