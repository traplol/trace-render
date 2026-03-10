#pragma once
#include "model/trace_model.h"
#include "model/query_db.h"
#include "ui/view_state.h"
#include <vector>
#include <string>
#include <nlohmann/json_fwd.hpp>

struct QueryTab {
    std::string title = "Query";
    std::string query;
    QueryDb::QueryResult result;
    bool has_result = false;

    // Instance browser for selected function name
    std::string selected_name;
    std::vector<uint32_t> instances;
    int32_t instance_cursor = -1;

    // Internal: non-serialized runtime state
    bool query_buf_dirty = true;  // need to copy query -> query_buf
    char query_buf[4096] = {};
};

class StatsPanel {
public:
    void render(const TraceModel& model, QueryDb& db, ViewState& view);

    nlohmann::json save_tabs() const;
    void load_tabs(const nlohmann::json& j);

private:
    static constexpr const char* DEFAULT_QUERY =
        "SELECT name, COUNT(*) as count, SUM(dur) as total_dur, AVG(dur) as avg_dur\n"
        "FROM events\n"
        "WHERE dur > 0\n"
        "GROUP BY name\n"
        "ORDER BY total_dur DESC\n"
        "LIMIT 50";

    std::vector<QueryTab> tabs_;
    int active_tab_ = 0;
    int32_t last_selected_event_ = -1;

    void ensure_default_tab();
    void render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view);
    void select_function_by_name(QueryTab& tab, const std::string& name, const TraceModel& model);
    void navigate_to_instance(QueryTab& tab, int32_t idx, const TraceModel& model, ViewState& view);
};
