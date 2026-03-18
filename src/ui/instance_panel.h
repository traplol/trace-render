#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <vector>
#include <string>

class InstancePanel {
public:
    void render(const TraceModel& model, ViewState& view);
    void reset();
    void on_model_changed();

private:
    std::string selected_name_;
    std::vector<uint32_t> instances_;
    int32_t instance_cursor_ = -1;
    int32_t last_selected_event_ = -1;
    bool instances_dirty_ = false;
    bool scroll_to_cursor_ = false;
    bool scroll_to_top_ = false;

    void select_function_by_name(const std::string& name, const TraceModel& model);
    void navigate_to_instance(int32_t idx, const TraceModel& model, ViewState& view);
};
