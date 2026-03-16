#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>
#include <vector>

// Flat node in an arena-allocated flame tree. All references are indices,
// never pointers, so the pool can grow freely and survive rebuilds.
struct FlameNode {
    uint32_t name_idx = 0;
    uint32_t cat_idx = 0;
    double total_time = 0.0;
    double self_time = 0.0;
    uint32_t call_count = 0;
    uint32_t first_child = UINT32_MAX;  // index into FlameTree::nodes
    uint32_t next_sibling = UINT32_MAX;
    uint32_t parent = UINT32_MAX;
};

struct FlameTree {
    uint32_t pid = 0;
    uint32_t tid = 0;
    std::string thread_name;
    double root_total_time = 0.0;
    uint32_t first_root = UINT32_MAX;  // index into nodes
    std::vector<FlameNode> nodes;

    const FlameNode& node(uint32_t idx) const { return nodes[idx]; }
};

class FlameGraphPanel {
public:
    void render(const TraceModel& model, ViewState& view);

    // Exposed for testing.
    void rebuild(const TraceModel& model, const ViewState& view);
    const std::vector<FlameTree>& trees() const { return trees_; }

private:
    std::vector<FlameTree> trees_;

    // Cache invalidation.
    size_t cached_event_count_ = 0;
    bool cached_has_range_ = false;
    double cached_range_start_ = 0.0;
    double cached_range_end_ = 0.0;
    size_t cached_hidden_hash_ = SIZE_MAX;

    // Per-tree zoom: index into that tree's nodes (UINT32_MAX = no zoom).
    std::vector<uint32_t> zoom_root_;
    int selected_tree_ = 0;
    char thread_filter_[128] = {};
    float sidebar_width_ = 180.0f;

    // Context menu state (indices, not pointers).
    uint32_t ctx_name_idx_ = UINT32_MAX;
    uint32_t ctx_cat_idx_ = UINT32_MAX;
    uint32_t ctx_node_idx_ = UINT32_MAX;  // into active tree's nodes
    int ctx_tree_idx_ = -1;

    bool needs_rebuild(const ViewState& view, size_t event_count) const;
    void update_cache_keys(const ViewState& view, size_t event_count);
    size_t compute_hidden_hash(const ViewState& view) const;

    void render_icicle(const TraceModel& model, ViewState& view, int tree_idx);

    // Tree building helpers.
    static uint32_t find_or_create_child(FlameTree& tree, uint32_t parent_idx, uint32_t name_idx, uint32_t cat_idx);
    static uint32_t find_or_create_root(FlameTree& tree, uint32_t name_idx, uint32_t cat_idx);
    static uint32_t sort_children(FlameTree& tree, uint32_t first_child);
    static void compute_self_times(FlameTree& tree);
};
