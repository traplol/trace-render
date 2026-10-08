#include "trace_model.h"
#include "tracing.h"
#include <algorithm>

void TraceModel::build_index(std::function<void(float)> on_progress) {
    TRACE_FUNCTION_CAT("model");

    resolve_stack_frames();
    min_ts_ = 1e18;
    max_ts_ = -1e18;

    // Rebuild from input order, including E records removed by a previous build.
    for (auto& proc : processes_)
        for (auto& thread : proc.threads) thread.event_indices.clear();
    for (uint32_t i = 0; i < events_.size(); ++i) {
        auto& ev = events_[i];
        ev.parent_idx = -1;
        ev.depth = 0;
        ev.self_time = 0;
        ev.is_end_event = false;
        if (ev.ph == Phase::Sample) {
            ev.kind = EventKind::Sample;
            ev.dur = 0;  // Observations never cover the interval to the next sample.
        }
        if (ev.ph != Phase::Metadata && ev.ph != Phase::Counter)
            get_or_create_process(ev.pid).get_or_create_thread(ev.tid).event_indices.push_back(i);
    }

    size_t total_threads = 0;
    for (const auto& proc : processes_) total_threads += proc.threads.size();
    size_t threads_done = 0;
    if (on_progress) on_progress(0.0f);

    std::vector<int32_t> begin_parent(events_.size(), -1);
    for (auto& proc : processes_) {
        for (auto& thread : proc.threads) {
            auto& indices = thread.event_indices;
            // Pair transitions in timestamp/input order before sorting by duration.
            std::sort(indices.begin(), indices.end(), [this](uint32_t a, uint32_t b) {
                if (events_[a].ts != events_[b].ts) return events_[a].ts < events_[b].ts;
                return a < b;
            });
            std::vector<uint32_t> measured_begins, sampled_begins;
            for (uint32_t idx : indices) {
                auto& ev = events_[idx];
                auto& begins = ev.kind == EventKind::SampledSpan ? sampled_begins : measured_begins;
                if (ev.ph == Phase::DurationBegin) {
                    if (!begins.empty()) begin_parent[idx] = (int32_t)begins.back();
                    begins.push_back(idx);
                } else if (ev.ph == Phase::DurationEnd && !begins.empty()) {
                    auto& begin = events_[begins.back()];
                    begin.dur = std::max(0.0, ev.ts - begin.ts);
                    ev.is_end_event = true;
                    begins.pop_back();
                }
            }
            indices.erase(
                std::remove_if(indices.begin(), indices.end(), [this](uint32_t i) { return events_[i].is_end_event; }),
                indices.end());

            // Equal intervals retain input order, including repeated recursive frames.
            std::sort(indices.begin(), indices.end(), [this](uint32_t a, uint32_t b) {
                if (events_[a].ts != events_[b].ts) return events_[a].ts < events_[b].ts;
                if (events_[a].dur != events_[b].dur) return events_[a].dur > events_[b].dur;
                return a < b;
            });

            std::vector<uint32_t> measured_stack, sampled_stack;
            thread.max_depth = 0;
            for (uint32_t idx : indices) {
                auto& ev = events_[idx];
                if (ev.kind == EventKind::Sample) continue;
                auto& stack = ev.kind == EventKind::SampledSpan ? sampled_stack : measured_stack;
                while (!stack.empty()) {
                    const auto& parent = events_[stack.back()];
                    if (parent.end_ts() <= ev.ts || parent.end_ts() < ev.end_ts())
                        stack.pop_back();
                    else
                        break;
                }
                if (ev.ph == Phase::DurationBegin && begin_parent[idx] >= 0) {
                    ev.parent_idx = begin_parent[idx];
                }
                if (!stack.empty()) {
                    // A B root cannot become a child of a different B merely because
                    // their zero-length boundary intervals coincide.
                    if ((ev.ph != Phase::DurationBegin || events_[stack.back()].ph != Phase::DurationBegin) &&
                        (ev.parent_idx < 0 || events_[stack.back()].depth >= events_[ev.parent_idx].depth))
                        ev.parent_idx = (int32_t)stack.back();
                }
                if (ev.parent_idx >= 0) ev.depth = (uint8_t)std::min(255, (int)events_[ev.parent_idx].depth + 1);
                thread.max_depth = std::max(thread.max_depth, ev.depth);
                ev.self_time = std::max(0.0, ev.dur);
                if (ev.dur > 0) stack.push_back(idx);
            }
            // Only actual immediate children subtract time. Samples use a separate
            // ancestry and must never subtract from measured spans.
            for (uint32_t idx : indices) {
                const auto& ev = events_[idx];
                if (ev.parent_idx >= 0 && ev.dur > 0) {
                    auto& parent = events_[ev.parent_idx];
                    parent.self_time = std::max(0.0, parent.self_time - ev.dur);
                }
            }
            thread.block_index.build(indices, events_);
            ++threads_done;
            if (on_progress && total_threads > 0) on_progress((float)threads_done / total_threads);
        }
    }

    // Sort threads and processes
    {
        for (auto& proc : processes_) {
            std::sort(proc.threads.begin(), proc.threads.end(), [](const ThreadInfo& a, const ThreadInfo& b) {
                if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
                return a.tid < b.tid;
            });
        }
        std::sort(processes_.begin(), processes_.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
            if (a.sort_index != b.sort_index) return a.sort_index < b.sort_index;
            return a.pid < b.pid;
        });
    }

    // Compute global time range, collect unique categories, and build name-to-events index
    {
        categories_.clear();
        name_to_events_.clear();
        std::unordered_set<uint32_t> cat_set;
        for (const auto& frame : stack_frames_)
            if (frame.valid) cat_set.insert(frame.cat_idx);
        for (uint32_t i = 0; i < (uint32_t)events_.size(); i++) {
            const auto& ev = events_[i];
            if (ev.ph == Phase::Metadata || ev.is_end_event) continue;
            if (ev.ts < min_ts_) min_ts_ = ev.ts;
            double end = ev.dur > 0 ? ev.end_ts() : ev.ts;
            if (end > max_ts_) max_ts_ = end;
            cat_set.insert(ev.cat_idx);
            if (ev.ph != Phase::Counter && (ev.dur > 0 || ev.kind == EventKind::Sample)) {
                name_to_events_[ev.name_idx].push_back(i);
            }
        }
        categories_.assign(cat_set.begin(), cat_set.end());
        std::sort(categories_.begin(), categories_.end(),
                  [this](uint32_t a, uint32_t b) { return strings_[a] < strings_[b]; });
        // Sort each name's event list by timestamp
        for (auto& [name_idx, indices] : name_to_events_) {
            std::sort(indices.begin(), indices.end(), [this](uint32_t a, uint32_t b) {
                if (events_[a].ts != events_[b].ts) return events_[a].ts < events_[b].ts;
                return a < b;
            });
        }
    }

    // Compute counter series min/max
    {
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

    // Cache aggregate stats for diagnostics panel (avoid per-frame O(n) scans)
    {
        cached_strings_bytes_ = 0;
        for (const auto& s : strings_) cached_strings_bytes_ += s.capacity();
        cached_args_bytes_ = 0;
        for (const auto& a : args_) cached_args_bytes_ += a.capacity();
        cached_counter_points_ = 0;
        for (const auto& cs : counter_series_) cached_counter_points_ += cs.points.size();
        cached_total_threads_ = 0;
        for (const auto& proc : processes_) cached_total_threads_ += (int)proc.threads.size();
    }
}

void TraceModel::resolve_stack_frames() {
    std::unordered_map<uint32_t, uint32_t> by_id;
    for (uint32_t i = 0; i < stack_frames_.size(); ++i) by_id[stack_frames_[i].id_idx] = i;
    for (auto& frame : stack_frames_) {
        frame.parent_idx = -1;
        if (frame.parent_id != UINT32_MAX) {
            auto it = by_id.find(frame.parent_id);
            if (it == by_id.end())
                frame.valid = false;
            else
                frame.parent_idx = (int32_t)it->second;
        }
    }
    // Iterative three-state walk detects cycles and missing ancestors without
    // recursion or quadratic walks through shared ancestry.
    std::vector<uint8_t> state(stack_frames_.size(), 0);
    std::vector<uint32_t> path;
    for (uint32_t i = 0; i < stack_frames_.size(); ++i) {
        if (state[i] == 2) continue;
        path.clear();
        int32_t current = (int32_t)i;
        while (current >= 0 && state[current] == 0 && stack_frames_[current].valid) {
            state[current] = 1;
            path.push_back(current);
            current = stack_frames_[current].parent_idx;
        }
        bool valid = current < 0 || (state[current] == 2 && stack_frames_[current].valid);
        for (uint32_t idx : path) {
            stack_frames_[idx].valid = valid;
            state[idx] = 2;
        }
        if (path.empty()) state[i] = 2;
    }
    for (auto& ev : events_) {
        ev.stack_frame_idx = -1;
        auto it = by_id.find(ev.stack_frame_id);
        if (it == by_id.end() || !stack_frames_[it->second].valid) continue;
        ev.stack_frame_idx = (int32_t)it->second;
        if (ev.ph == Phase::Sample) {
            const auto& leaf = stack_frames_[it->second];
            ev.name_idx = leaf.name_idx;
            if (leaf.cat_idx != 0) ev.cat_idx = leaf.cat_idx;
        }
    }
}

std::vector<uint32_t> TraceModel::build_sample_stack(uint32_t event_idx) const {
    std::vector<uint32_t> path;
    if (event_idx >= events_.size()) return path;
    for (int32_t i = events_[event_idx].stack_frame_idx; i >= 0; i = stack_frames_[i].parent_idx)
        path.push_back((uint32_t)i);
    std::reverse(path.begin(), path.end());
    return path;
}

int32_t TraceModel::find_parent_event(uint32_t event_idx) const {
    if (event_idx >= events_.size()) return -1;
    return events_[event_idx].parent_idx;
}

std::vector<uint32_t> TraceModel::build_call_stack(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    std::vector<uint32_t> stack;
    if (event_idx >= events_.size()) return stack;

    // Walk up pre-computed parent chain
    uint32_t current = event_idx;
    while (current < events_.size()) {
        stack.push_back(current);
        int32_t parent = events_[current].parent_idx;
        if (parent < 0) break;
        current = (uint32_t)parent;
    }
    // Reverse so root is first, selected event is last
    std::reverse(stack.begin(), stack.end());
    return stack;
}

double TraceModel::compute_self_time(uint32_t event_idx) const {
    if (event_idx >= events_.size()) return 0.0;
    return events_[event_idx].self_time;
}

int32_t TraceModel::find_longest_child(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    if (event_idx >= events_.size()) return -1;
    const auto& ev = events_[event_idx];
    const auto* thread = find_thread(ev.pid, ev.tid);
    if (!thread) return -1;
    uint8_t child_depth = ev.depth + 1;
    int32_t best_idx = -1;
    double best_dur = -1.0;
    for (uint32_t idx : thread->event_indices) {
        const auto& candidate = events_[idx];
        if (candidate.ts < ev.ts) continue;
        if (candidate.ts >= ev.end_ts()) break;
        if (candidate.depth == child_depth && candidate.parent_idx == (int32_t)event_idx) {
            if (candidate.dur > best_dur) {
                best_dur = candidate.dur;
                best_idx = (int32_t)idx;
            }
        }
    }
    return best_idx;
}

int32_t TraceModel::find_prev_sibling(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    if (event_idx >= events_.size()) return -1;
    const auto& ev = events_[event_idx];
    const auto* thread = find_thread(ev.pid, ev.tid);
    if (!thread) return -1;
    int32_t result = -1;
    for (uint32_t idx : thread->event_indices) {
        const auto& candidate = events_[idx];
        if (candidate.depth == ev.depth && candidate.parent_idx == ev.parent_idx) {
            if (idx == event_idx) return result;
            result = (int32_t)idx;
        }
    }
    return -1;
}

int32_t TraceModel::find_next_sibling(uint32_t event_idx) const {
    TRACE_FUNCTION_CAT("model");
    if (event_idx >= events_.size()) return -1;
    const auto& ev = events_[event_idx];
    const auto* thread = find_thread(ev.pid, ev.tid);
    if (!thread) return -1;
    bool found = false;
    for (uint32_t idx : thread->event_indices) {
        const auto& candidate = events_[idx];
        if (candidate.depth == ev.depth && candidate.parent_idx == ev.parent_idx) {
            if (found) return (int32_t)idx;
            if (idx == event_idx) found = true;
        }
    }
    return -1;
}
