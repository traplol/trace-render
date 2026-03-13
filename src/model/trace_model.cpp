#include "trace_model.h"
#include "tracing.h"
#include <stack>
#include <algorithm>

void TraceModel::build_index() {
    TRACE_SCOPE_CAT("BuildIndex", "model");
    // Sort events within each thread by timestamp
    for (auto& proc : processes_) {
        for (auto& thread : proc.threads) {
            std::sort(thread.event_indices.begin(), thread.event_indices.end(),
                      [this](uint32_t a, uint32_t b) { return events_[a].ts < events_[b].ts; });
        }
    }

    // Match B/E pairs and compute duration
    for (auto& proc : processes_) {
        for (auto& thread : proc.threads) {
            std::stack<uint32_t> begin_stack;
            for (uint32_t idx : thread.event_indices) {
                auto& ev = events_[idx];
                if (ev.ph == Phase::DurationBegin) {
                    begin_stack.push(idx);
                } else if (ev.ph == Phase::DurationEnd) {
                    if (!begin_stack.empty()) {
                        auto& begin_ev = events_[begin_stack.top()];
                        begin_ev.dur = ev.ts - begin_ev.ts;
                        ev.is_end_event = true;
                        begin_stack.pop();
                    }
                }
            }
        }
    }

    // Remove end events from thread indices and compute depth
    for (auto& proc : processes_) {
        for (auto& thread : proc.threads) {
            // Remove matched end events
            thread.event_indices.erase(std::remove_if(thread.event_indices.begin(), thread.event_indices.end(),
                                                      [this](uint32_t idx) {
                                                          const auto& ev = events_[idx];
                                                          return ev.is_end_event || ev.ph == Phase::Metadata;
                                                      }),
                                       thread.event_indices.end());

            // Deduplicate events with the same name and timestamp (keep longer duration)
            if (thread.event_indices.size() > 1) {
                std::vector<uint32_t> deduped;
                deduped.reserve(thread.event_indices.size());
                deduped.push_back(thread.event_indices[0]);
                for (size_t i = 1; i < thread.event_indices.size(); i++) {
                    uint32_t prev_idx = deduped.back();
                    uint32_t cur_idx = thread.event_indices[i];
                    const auto& prev = events_[prev_idx];
                    const auto& cur = events_[cur_idx];
                    if (cur.name_idx == prev.name_idx && cur.ts == prev.ts) {
                        // Keep the one with longer duration
                        if (cur.dur > prev.dur) {
                            deduped.back() = cur_idx;
                        }
                    } else {
                        deduped.push_back(cur_idx);
                    }
                }
                thread.event_indices = std::move(deduped);
            }

            // Compute nesting depth using a stack of end timestamps
            std::vector<double> depth_stack;
            uint8_t max_depth = 0;
            for (uint32_t idx : thread.event_indices) {
                auto& ev = events_[idx];
                // Pop entries that have ended before this event starts
                while (!depth_stack.empty() && depth_stack.back() <= ev.ts) {
                    depth_stack.pop_back();
                }
                ev.depth = (uint8_t)depth_stack.size();
                if (ev.depth > max_depth) max_depth = ev.depth;
                if (ev.dur > 0) {
                    depth_stack.push_back(ev.end_ts());
                }
            }
            thread.max_depth = max_depth;

            // Build spatial index
            thread.block_index.build(thread.event_indices, events_);
        }
    }

    // Sort threads within processes
    for (auto& proc : processes_) {
        std::sort(proc.threads.begin(), proc.threads.end(), [](const ThreadInfo& a, const ThreadInfo& b) {
            if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
            return a.tid < b.tid;
        });
    }

    // Sort processes
    std::sort(processes_.begin(), processes_.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
        return a.pid < b.pid;
    });

    // Compute global time range
    for (const auto& ev : events_) {
        if (ev.ph == Phase::Metadata || ev.is_end_event) continue;
        if (ev.ts < min_ts_) min_ts_ = ev.ts;
        double end = ev.dur > 0 ? ev.end_ts() : ev.ts;
        if (end > max_ts_) max_ts_ = end;
    }

    // Compute counter series min/max
    for (auto& cs : counter_series_) {
        if (cs.points.empty()) continue;
        std::sort(cs.points.begin(), cs.points.end());
        cs.min_val = cs.points[0].second;
        cs.max_val = cs.points[0].second;
        for (const auto& pt : cs.points) {
            cs.min_val = std::min(cs.min_val, pt.second);
            cs.max_val = std::max(cs.max_val, pt.second);
        }
    }
}

int32_t TraceModel::find_parent_event(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    if (event_idx >= events_.size()) return -1;
    const auto& ev = events_[event_idx];
    if (ev.depth == 0) return -1;

    const auto* proc = find_process(ev.pid);
    if (!proc) return -1;
    const auto* thread = proc->find_thread(ev.tid);
    if (!thread) return -1;

    uint8_t parent_depth = ev.depth - 1;
    for (uint32_t idx : thread->event_indices) {
        const auto& candidate = events_[idx];
        if (candidate.depth == parent_depth && candidate.ts <= ev.ts && candidate.end_ts() >= ev.end_ts()) {
            return (int32_t)idx;
        }
    }
    return -1;
}

std::vector<uint32_t> TraceModel::build_call_stack(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    std::vector<uint32_t> stack;
    if (event_idx >= events_.size()) return stack;

    stack.push_back(event_idx);
    uint32_t current = event_idx;
    while (true) {
        int32_t parent = find_parent_event(current);
        if (parent < 0) break;
        stack.push_back((uint32_t)parent);
        current = (uint32_t)parent;
    }
    // Reverse so root is first, selected event is last
    std::reverse(stack.begin(), stack.end());
    return stack;
}

double TraceModel::compute_self_time(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    if (event_idx >= events_.size()) return 0.0;
    const auto& ev = events_[event_idx];
    if (ev.dur <= 0) return 0.0;

    const auto* proc = find_process(ev.pid);
    if (!proc) return ev.dur;
    const auto* thread = proc->find_thread(ev.tid);
    if (!thread) return ev.dur;

    double children_total = 0.0;
    for (uint32_t idx : thread->event_indices) {
        const auto& child = events_[idx];
        if (child.depth != ev.depth + 1) continue;
        if (child.ts < ev.ts) continue;
        if (child.ts > ev.end_ts()) break;
        if (child.end_ts() > ev.end_ts()) continue;
        if (child.dur > 0) children_total += child.dur;
    }
    return ev.dur - children_total;
}
