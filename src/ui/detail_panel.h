#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class DetailPanel {
public:
    void render(const TraceModel& model, ViewState& view);
};
