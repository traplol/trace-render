#pragma once
#include "model/trace_model.h"
#include <vector>

struct RangeEventSummary {
    uint32_t name_idx;
    uint32_t count;
    double total_dur;
    double min_dur;
    double max_dur;
    uint32_t longest_idx;  // event index of longest instance (by actual ev.dur)

    double avg_dur() const { return count > 0 ? total_dur / count : 0.0; }
};

struct RangeStats {
    double range_duration = 0.0;
    uint32_t total_events = 0;
    std::vector<RangeEventSummary> summaries;  // sorted by total_dur descending
};

// Compute statistics for all events overlapping the given time range.
// Only considers events with dur > 0 (not end events).
// Contributions are clamped to the range, but longest_idx tracks by actual ev.dur.
RangeStats compute_range_stats(const TraceModel& model, double start_ts, double end_ts);
