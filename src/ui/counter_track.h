#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include "imgui.h"

class CounterTrackRenderer {
public:
    // Renders all counter tracks for a given process below the thread tracks.
    // Returns the total height consumed.
    float render(ImDrawList* dl, ImVec2 area_min, float y_offset, float width, const TraceModel& model, uint32_t pid,
                 const ViewState& view);

    // Render a single counter series
    void render_series(ImDrawList* dl, ImVec2 track_min, ImVec2 track_max, const CounterSeries& series,
                       const ViewState& view, ImU32 color);
};
