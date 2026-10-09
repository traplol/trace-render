#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"

class MemoryPanel {
public:
    void render(const TraceModel& model, ViewState& view);
    void on_model_changed();
    void set_time(double ts);
    void set_process(std::string process_id);
    void set_birth_range(std::optional<std::pair<double, double>> range);
    void refresh(const TraceModel& model);
    void select_function(const TraceModel& model, int32_t row);
    void select_stack(const TraceModel& model, int32_t row);

    double time() const { return time_; }
    const std::string& process_id() const { return process_id_; }
    const std::optional<std::pair<double, double>>& birth_range() const { return birth_range_; }
    const OutstandingMemory& result() const { return result_; }
    int32_t selected_function() const { return selected_function_; }
    int32_t selected_stack() const { return selected_stack_; }
    const std::vector<size_t>& contributing_stacks() const { return contributing_stacks_; }
    const std::vector<int32_t>& selected_path() const { return selected_path_; }
    void select_snapshot(size_t index) { selected_snapshot_ = index; }
    size_t selected_snapshot() const { return selected_snapshot_; }

private:
    void render_snapshot(const TraceModel& model);
    bool snapshot_mode_ = false;
    size_t selected_snapshot_ = 0;
    bool initialized_ = false;
    bool dirty_ = true;
    double time_ = 0;
    std::string process_id_;
    std::optional<std::pair<double, double>> birth_range_;
    OutstandingMemory result_;
    std::vector<std::string> stack_labels_;
    int32_t selected_function_ = -1;
    int32_t selected_stack_ = -1;
    std::vector<size_t> contributing_stacks_;
    std::vector<int32_t> selected_path_;
};
