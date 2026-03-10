#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class FilterPanel {
public:
    void render(const TraceModel& model, ViewState& view);
};
