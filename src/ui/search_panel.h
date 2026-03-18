#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class SearchPanel {
public:
    void render(const TraceModel& model, ViewState& view);
    void on_model_changed();

private:
    // NOTE: update reset() when adding cached fields
    void reset();
    char search_buf_[256] = {};
    bool needs_search_ = false;

    // Sorted view of search results
    std::vector<uint32_t> sorted_results_;
    bool needs_sort_ = false;
    bool scroll_to_top_ = false;
};
