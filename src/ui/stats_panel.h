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
    bool query_running = false;

    // Internal: non-serialized runtime state
    bool query_buf_dirty = true;
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

    bool show_schema_ = false;

    void ensure_default_tab();
    void render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view);
    void render_schema_popup(QueryDb& db);
};
