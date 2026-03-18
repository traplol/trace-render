#pragma once
#include "trace_event.h"
#include "block_index.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <climits>
#include <functional>
#include <utility>

struct ThreadInfo {
    uint32_t tid = 0;
    std::string name;
    std::vector<uint32_t> event_indices;  // indices into TraceModel::events_, sorted by ts
    int32_t sort_index = 0;
    uint8_t max_depth = 0;
    BlockIndex block_index;
};

struct ProcessInfo {
    uint32_t pid = 0;
    std::string name;
    std::vector<ThreadInfo> threads;
    int32_t sort_index = 0;

    const ThreadInfo* find_thread(uint32_t tid) const {
        for (const auto& t : threads) {
            if (t.tid == tid) return &t;
        }
        return nullptr;
    }

    ThreadInfo* find_thread(uint32_t tid) { return const_cast<ThreadInfo*>(std::as_const(*this).find_thread(tid)); }

    ThreadInfo& get_or_create_thread(uint32_t tid) {
        if (auto* t = find_thread(tid)) return *t;
        threads.push_back({});
        threads.back().tid = tid;
        threads.back().name = "Thread " + std::to_string(tid);
        return threads.back();
    }
};

struct CounterSeries {
    uint32_t pid = 0;
    std::string name;
    std::vector<std::pair<double, double>> points;  // (timestamp, value)
    double min_val = 0.0;
    double max_val = 0.0;
};

class TraceModel {
public:
    // --- Const accessors ---
    const std::vector<TraceEvent>& events() const { return events_; }
    const std::vector<std::string>& strings() const { return strings_; }
    const std::unordered_map<std::string, uint32_t>& string_map() const { return string_map_; }
    const std::vector<std::string>& args() const { return args_; }
    const std::vector<ProcessInfo>& processes() const { return processes_; }
    const std::vector<CounterSeries>& counter_series() const { return counter_series_; }
    const std::unordered_map<uint64_t, std::vector<uint32_t>>& flow_groups() const { return flow_groups_; }
    double min_ts() const { return min_ts_; }
    double max_ts() const { return max_ts_; }
    const std::vector<uint32_t>& categories() const { return categories_; }
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& name_to_events() const { return name_to_events_; }

    // Aggregate stats (computed once in build_index)
    size_t strings_bytes() const { return cached_strings_bytes_; }
    size_t args_bytes() const { return cached_args_bytes_; }
    size_t counter_points_count() const { return cached_counter_points_; }
    int total_threads() const { return cached_total_threads_; }

    // --- Mutation methods for building the model ---
    uint32_t add_event(const TraceEvent& ev) {
        uint32_t idx = (uint32_t)events_.size();
        events_.push_back(ev);
        return idx;
    }

    uint32_t add_args(std::string args_json) {
        uint32_t idx = (uint32_t)args_.size();
        args_.push_back(std::move(args_json));
        return idx;
    }

    void add_flow_event(uint64_t id, uint32_t event_idx) { flow_groups_[id].push_back(event_idx); }

    CounterSeries& find_or_create_counter_series(uint32_t pid, const std::string& name) {
        for (auto& s : counter_series_) {
            if (s.pid == pid && s.name == name) return s;
        }
        counter_series_.push_back({});
        auto& cs = counter_series_.back();
        cs.pid = pid;
        cs.name = name;
        return cs;
    }

    uint32_t intern_string(const std::string& s) {
        auto it = string_map_.find(s);
        if (it != string_map_.end()) return it->second;
        uint32_t idx = (uint32_t)strings_.size();
        strings_.push_back(s);
        string_map_[s] = idx;
        return idx;
    }

    const std::string& get_string(uint32_t idx) const {
        static const std::string empty;
        if (idx >= strings_.size()) return empty;
        return strings_[idx];
    }

    const ProcessInfo* find_process(uint32_t pid) const {
        for (const auto& p : processes_) {
            if (p.pid == pid) return &p;
        }
        return nullptr;
    }

    ProcessInfo* find_process(uint32_t pid) { return const_cast<ProcessInfo*>(std::as_const(*this).find_process(pid)); }

    const ThreadInfo* find_thread(uint32_t pid, uint32_t tid) const {
        if (const auto* proc = find_process(pid)) return proc->find_thread(tid);
        return nullptr;
    }

    ThreadInfo* find_thread(uint32_t pid, uint32_t tid) {
        return const_cast<ThreadInfo*>(std::as_const(*this).find_thread(pid, tid));
    }

    ProcessInfo& get_or_create_process(uint32_t pid) {
        if (auto* p = find_process(pid)) return *p;
        processes_.push_back({});
        processes_.back().pid = pid;
        processes_.back().name = "Process " + std::to_string(pid);
        return processes_.back();
    }

    void build_index(std::function<void(float)> on_progress = nullptr);

    // Find the parent event (depth-1, same thread, enclosing time range).
    // Returns -1 if no parent found.
    int32_t find_parent_event(uint32_t event_idx) const;

    // Build the full call stack from the given event up to the root.
    // Returns event indices ordered from root (index 0) to the given event (last element).
    std::vector<uint32_t> build_call_stack(uint32_t event_idx) const;

    // Compute self time for an event (wall time minus immediate children's durations).
    double compute_self_time(uint32_t event_idx) const;

    // Navigate to the longest immediate child of an event. Returns -1 if none found.
    int32_t find_longest_child(uint32_t event_idx) const;

    // Navigate to the previous sibling (same parent, earlier timestamp). Returns -1 if none found.
    int32_t find_prev_sibling(uint32_t event_idx) const;

    // Navigate to the next sibling (same parent, later timestamp). Returns -1 if none found.
    int32_t find_next_sibling(uint32_t event_idx) const;

    void query_visible(const ThreadInfo& thread, double start_ts, double end_ts, std::vector<uint32_t>& out) const {
        thread.block_index.query(start_ts, end_ts, thread.event_indices, events_, out);
    }

    void clear() {
        events_.clear();
        strings_.clear();
        string_map_.clear();
        args_.clear();
        processes_.clear();
        counter_series_.clear();
        flow_groups_.clear();
        categories_.clear();
        name_to_events_.clear();
        min_ts_ = 1e18;
        max_ts_ = -1e18;
        cached_strings_bytes_ = 0;
        cached_args_bytes_ = 0;
        cached_counter_points_ = 0;
        cached_total_threads_ = 0;
    }

private:
    std::vector<TraceEvent> events_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> string_map_;
    std::vector<std::string> args_;
    std::vector<ProcessInfo> processes_;
    std::vector<CounterSeries> counter_series_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> flow_groups_;
    double min_ts_ = 1e18;
    double max_ts_ = -1e18;
    std::vector<uint32_t> categories_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> name_to_events_;

    // Cached aggregate stats for diagnostics panel
    size_t cached_strings_bytes_ = 0;
    size_t cached_args_bytes_ = 0;
    size_t cached_counter_points_ = 0;
    int cached_total_threads_ = 0;
};
