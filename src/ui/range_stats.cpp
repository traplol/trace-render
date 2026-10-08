#include "range_stats.h"
#include <algorithm>
#include <unordered_map>
#include "tracing.h"

RangeStats compute_range_stats(const TraceModel& model, double start_ts, double end_ts) {
    TRACE_FUNCTION_CAT("ui");
    RangeStats stats;
    stats.range_duration = end_ts - start_ts;

    std::unordered_map<uint64_t, size_t> name_to_idx;
    std::unordered_map<uint32_t, size_t> sample_names;

    for (const auto& proc : model.processes()) {
        for (const auto& thread : proc.threads) {
            std::vector<uint32_t> candidates;
            model.query_visible(thread, start_ts, end_ts, candidates);

            for (uint32_t idx : candidates) {
                const auto& ev = model.events()[idx];
                if (ev.is_end_event) continue;
                if (ev.kind == EventKind::Sample) {
                    if (ev.ts < start_ts || ev.ts >= end_ts) continue;
                    ++stats.total_samples;
                    std::vector<uint32_t> names;
                    for (uint32_t f : model.build_sample_stack(idx)) names.push_back(model.stack_frames()[f].name_idx);
                    if (names.empty()) names.push_back(ev.name_idx);
                    uint32_t leaf_name = names.back();
                    // One observation contributes once per function, even in recursion.
                    std::sort(names.begin(), names.end());
                    names.erase(std::unique(names.begin(), names.end()), names.end());
                    for (uint32_t name : names) {
                        auto [it, inserted] = sample_names.emplace(name, stats.sample_summaries.size());
                        if (inserted) stats.sample_summaries.push_back({name, 0, 0, 0, 0, 0, idx});
                        auto& summary = stats.sample_summaries[it->second];
                        ++summary.inclusive_samples;
                        if (name == leaf_name) ++summary.exclusive_samples;
                        if (ev.sample_cpu_time >= 0) {
                            ++summary.weighted_samples;
                            summary.estimated_cpu_time += ev.sample_cpu_time;
                            if (name == leaf_name) summary.estimated_self_cpu_time += ev.sample_cpu_time;
                        }
                    }
                    continue;
                }
                if (ev.dur <= 0) continue;

                // Clamp event to range for contribution calculation
                double ev_start = std::max(ev.ts, start_ts);
                double ev_end = std::min(ev.end_ts(), end_ts);
                double contribution = ev_end - ev_start;
                if (contribution <= 0) continue;

                if (ev.kind == EventKind::Measured)
                    ++stats.total_events;
                else
                    ++stats.total_sampled_spans;

                uint64_t key = ((uint64_t)ev.kind << 32) | ev.name_idx;
                auto it = name_to_idx.find(key);
                if (it == name_to_idx.end()) {
                    name_to_idx[key] = stats.summaries.size();
                    stats.summaries.push_back({ev.name_idx, 1, contribution, contribution, contribution, idx, ev.kind});
                } else {
                    auto& s = stats.summaries[it->second];
                    s.count++;
                    s.total_dur += contribution;
                    if (contribution < s.min_dur) s.min_dur = contribution;
                    if (contribution > s.max_dur) s.max_dur = contribution;
                    // Track longest by actual event duration, not clamped contribution
                    if (ev.dur > model.events()[s.longest_idx].dur) {
                        s.longest_idx = idx;
                    }
                }
            }
        }
    }

    // Sort by total duration descending
    std::sort(stats.summaries.begin(), stats.summaries.end(),
              [](const RangeEventSummary& a, const RangeEventSummary& b) {
                  if (a.kind != b.kind) return a.kind < b.kind;
                  return a.total_dur > b.total_dur;
              });
    std::sort(stats.sample_summaries.begin(), stats.sample_summaries.end(),
              [](const RangeSampleSummary& a, const RangeSampleSummary& b) {
                  if (a.inclusive_samples != b.inclusive_samples) return a.inclusive_samples > b.inclusive_samples;
                  return a.name_idx < b.name_idx;
              });

    return stats;
}
