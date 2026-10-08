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
    EventKind kind = EventKind::Measured;

    double avg_dur() const { return count > 0 ? total_dur / count : 0.0; }
};

struct RangeSampleSummary {
    uint32_t name_idx = 0;
    uint32_t inclusive_samples = 0;
    uint32_t exclusive_samples = 0;
    uint32_t weighted_samples = 0;
    double estimated_cpu_time = 0;
    double estimated_self_cpu_time = 0;
    uint32_t event_idx = 0;
};

struct RangeStats {
    double range_duration = 0.0;
    uint32_t total_events = 0;
    uint32_t total_sampled_spans = 0;
    uint32_t total_samples = 0;
    std::vector<RangeEventSummary> summaries;  // sorted by total_dur descending
    std::vector<RangeSampleSummary> sample_summaries;
};

// Compute statistics for all events overlapping the given time range.
// Measured spans and sampled spans are grouped separately. Samples use [start, end).
// Contributions are clamped to the range, but longest_idx tracks by actual ev.dur.
RangeStats compute_range_stats(const TraceModel& model, double start_ts, double end_ts);
