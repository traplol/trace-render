#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <vector>

class FlameGraphPanel {
public:
    void render(const TraceModel& model, ViewState& view);

    struct FlameNode {
        uint32_t name_idx = 0;
        uint32_t cat_idx = 0;
        double total_time = 0.0;
        double self_time = 0.0;
        uint32_t call_count = 0;
        std::vector<size_t> children;  // indices into nodes_
    };

    // Exposed for testing
    const std::vector<FlameNode>& nodes() const { return nodes_; }
    size_t root() const { return root_idx_; }
    void rebuild(const TraceModel& model, const ViewState& view);

private:
    std::vector<FlameNode> nodes_;
    size_t root_idx_ = 0;

    std::vector<size_t> zoom_stack_;
    size_t zoom_root_ = 0;
    uint32_t zoom_name_idx_ = UINT32_MAX;
    uint32_t zoom_cat_idx_ = UINT32_MAX;

    double cached_range_start_ = -1.0;
    double cached_range_end_ = -1.0;
    bool cached_has_range_ = false;
    size_t cached_event_count_ = 0;
    uint32_t cached_filter_gen_ = UINT32_MAX;

    bool icicle_mode_ = false;

    void render_bars(const TraceModel& model, ViewState& view);
    void sort_node(size_t idx);
    size_t find_node(uint32_t name_idx, uint32_t cat_idx) const;
};
