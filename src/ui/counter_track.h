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

// A segment produced by sub-pixel merging of counter points.
// When multiple points map to the same pixel column, they are merged into a
// single segment with the min/max range and the last value for continuity.
struct MergedCounterSegment {
    float x;
    double min_val;
    double max_val;
    double last_val;
    int point_count;  // 1 = single point, >1 = merged bucket
};

// Merge counter points that fall on the same pixel column.
// view_start/view_end define the visible time range.
// track_x/track_w define the pixel mapping.
// Includes one point before view_start and one past view_end for continuity.
std::vector<MergedCounterSegment> merge_counter_points(const std::vector<std::pair<double, double>>& points,
                                                       double view_start, double view_end, float track_x,
                                                       float track_w);

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
