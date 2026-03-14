#pragma once
#include "trace_event.h"
#include "block_index.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <set>
#include <climits>
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
    // All events
    std::vector<TraceEvent> events_;

    // String pool
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> string_map_;

    // Args storage (serialized JSON)
    std::vector<std::string> args_;

    // Process/thread hierarchy
    std::vector<ProcessInfo> processes_;

    // Counter series keyed by "pid:name"
    std::vector<CounterSeries> counter_series_;

    // Flow events grouped by id
    std::unordered_map<uint64_t, std::vector<uint32_t>> flow_groups_;

    // Global time range
    double min_ts_ = 1e18;
    double max_ts_ = -1e18;

    // Pre-computed unique category indices (built in build_index)
    std::set<uint32_t> categories_;

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

    void build_index();

    // Find the parent event (depth-1, same thread, enclosing time range).
    // Returns -1 if no parent found.
    int32_t find_parent_event(uint32_t event_idx) const;

    // Build the full call stack from the given event up to the root.
    // Returns event indices ordered from root (index 0) to the given event (last element).
    std::vector<uint32_t> build_call_stack(uint32_t event_idx) const;

    // Compute self time for an event (wall time minus immediate children's durations).
    double compute_self_time(uint32_t event_idx) const;

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
        min_ts_ = 1e18;
        max_ts_ = -1e18;
    }
};
