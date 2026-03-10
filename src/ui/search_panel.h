#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class SearchPanel {
public:
    void render(const TraceModel& model, ViewState& view);

private:
    char search_buf_[256] = {};
    bool needs_search_ = false;
};
