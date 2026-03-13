#include "range_stats.h"
#include <algorithm>
#include <unordered_map>

RangeStats compute_range_stats(const TraceModel& model, double start_ts, double end_ts) {
    RangeStats stats;
    stats.range_duration = end_ts - start_ts;

    std::unordered_map<uint32_t, size_t> name_to_idx;

    for (const auto& proc : model.processes_) {
        for (const auto& thread : proc.threads) {
            std::vector<uint32_t> candidates;
            model.query_visible(thread, start_ts, end_ts, candidates);

            for (uint32_t idx : candidates) {
                const auto& ev = model.events_[idx];
                if (ev.is_end_event) continue;
                if (ev.dur <= 0) continue;

                // Clamp event to range for contribution calculation
                double ev_start = std::max(ev.ts, start_ts);
                double ev_end = std::min(ev.end_ts(), end_ts);
                double contribution = ev_end - ev_start;
                if (contribution <= 0) continue;

                stats.total_events++;

                auto it = name_to_idx.find(ev.name_idx);
                if (it == name_to_idx.end()) {
                    name_to_idx[ev.name_idx] = stats.summaries.size();
                    stats.summaries.push_back({ev.name_idx, 1, contribution, contribution, contribution, idx});
                } else {
                    auto& s = stats.summaries[it->second];
                    s.count++;
                    s.total_dur += contribution;
                    if (contribution < s.min_dur) s.min_dur = contribution;
                    if (contribution > s.max_dur) s.max_dur = contribution;
                    // Track longest by actual event duration, not clamped contribution
                    if (ev.dur > model.events_[s.longest_idx].dur) {
                        s.longest_idx = idx;
                    }
                }
            }
        }
    }

    // Sort by total duration descending
    std::sort(stats.summaries.begin(), stats.summaries.end(),
              [](const RangeEventSummary& a, const RangeEventSummary& b) { return a.total_dur > b.total_dur; });

    return stats;
}
