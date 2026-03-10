#pragma once
#include "model/trace_model.h"
#include "model/query_db.h"
#include "ui/view_state.h"
#include <vector>
#include <string>

class StatsPanel {
public:
    void render(const TraceModel& model, QueryDb& db, ViewState& view);

private:
    // SQL query editor
    char query_buf_[4096] = "SELECT name, COUNT(*) as count, SUM(dur) as total_dur, AVG(dur) as avg_dur\nFROM events\nWHERE dur > 0\nGROUP BY name\nORDER BY total_dur DESC\nLIMIT 50";
    QueryDb::QueryResult result_;
    bool has_result_ = false;

    // Instance browser for selected function name
    std::string selected_name_;
    std::vector<uint32_t> instances_;
    int32_t instance_cursor_ = -1;

    void select_function_by_name(const std::string& name, const TraceModel& model);
    void navigate_to_instance(int32_t idx, const TraceModel& model, ViewState& view);
};
