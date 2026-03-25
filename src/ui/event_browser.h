#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>
#include <vector>

// Reusable sortable/filterable event list with optional group-by-name aggregation.
// Used by DetailPanel for children tab, siblings tab, and any future event lists.
class EventBrowser {
public:
    struct Entry {
        uint32_t event_idx;
        uint32_t name_idx;
        double dur;
        float pct;
    };

    struct Aggregated {
        uint32_t name_idx;
        uint32_t count;
        double total_dur;
        double avg_dur;
        double min_dur;
        double max_dur;
        float pct;
        uint32_t longest_idx;
    };

    explicit EventBrowser(bool default_group_by_name = false);

    // Replace entries and rebuild aggregation + filter.
    void set_entries(std::vector<Entry> entries, double parent_dur, const TraceModel& model);

    // Renders group-by-name checkbox, filter input, and the appropriate table.
    void render(const char* id, const TraceModel& model, ViewState& view);

    size_t entry_count() const { return entries_.size(); }
    void reset();

private:
    std::vector<Entry> entries_;
    std::vector<Aggregated> aggregated_;
    std::vector<size_t> filtered_;
    std::vector<size_t> filtered_agg_;
    double parent_dur_ = 0.0;
    bool dirty_ = false;
    bool group_by_name_;
    bool default_group_by_name_;
    bool cached_group_ = false;
    char filter_buf_[256] = {};
    std::string active_filter_;
    bool scroll_to_top_ = false;
    bool scroll_agg_to_top_ = false;

    void rebuild_aggregated(const TraceModel& model);
    void rebuild_filter(const TraceModel& model);
    void render_table(const TraceModel& model, ViewState& view);
    void render_aggregated_table(const TraceModel& model, ViewState& view);
};
