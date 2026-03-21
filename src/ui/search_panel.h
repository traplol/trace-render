#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <unordered_map>

struct NameStats {
    uint32_t count = 0;
    double total_dur = 0.0;
    double avg_dur = 0.0;
};

class SearchPanel {
public:
    void render(const TraceModel& model, ViewState& view);
    void on_model_changed();
    void build_name_stats(const TraceModel& model, const std::vector<uint32_t>& results);
    const std::unordered_map<uint32_t, NameStats>& name_stats() const { return name_stats_; }

    bool unique_by_name() const { return unique_by_name_; }
    void set_unique_by_name(bool v) { unique_by_name_ = v; }

    // Filter results to keep one event per unique name (first occurrence).
    static std::vector<uint32_t> filter_unique_by_name(const TraceModel& model, const std::vector<uint32_t>& results);

private:
    // NOTE: update reset() when adding cached fields
    void reset();
    char search_buf_[256] = {};
    bool needs_search_ = false;
    bool unique_by_name_ = true;

    // Sorted view of search results (may be filtered by unique_by_name_)
    std::vector<uint32_t> sorted_results_;
    bool needs_sort_ = false;
    bool scroll_to_top_ = false;

    // Per-name aggregates (name_idx -> stats)
    std::unordered_map<uint32_t, NameStats> name_stats_;
};
