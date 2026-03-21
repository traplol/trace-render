#pragma once
#include "model/trace_model.h"
#include "model/query_db.h"
#include "ui/view_state.h"
#include <vector>
#include <string>
#include <nlohmann/json_fwd.hpp>

struct SDL_Window;

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

// Query builder state
struct QueryBuilderState {
    // Table
    int table_idx = 0;  // 0=events, 1=processes, 2=threads, 3=counters

    // SELECT columns: index into current table's column list
    // -1 = raw column, >=0 = aggregate function index
    struct SelectCol {
        int col_idx = 0;
        int agg_idx = 0;  // 0=none, 1=COUNT, 2=SUM, 3=AVG, 4=MIN, 5=MAX
        char alias[64] = {};
    };
    std::vector<SelectCol> select_cols;

    // WHERE conditions
    struct WhereClause {
        int col_idx = 0;
        int op_idx =
            0;  // 0: =, 1: !=, 2: <, 3: >, 4: <=, 5: >=, 6: LIKE, 7: NOT LIKE, 8: IN, 9: IS NULL, 10: IS NOT NULL
        char value[256] = {};
        int logic_idx = 0;  // 0=AND, 1=OR
    };
    std::vector<WhereClause> where_clauses;

    // GROUP BY
    std::vector<int> group_cols;  // column indices

    // HAVING (only when GROUP BY is set)
    struct HavingClause {
        int agg_idx = 1;  // 1=COUNT, 2=SUM, 3=AVG, 4=MIN, 5=MAX
        int col_idx = 0;
        int op_idx = 0;
        char value[128] = {};
    };
    std::vector<HavingClause> having_clauses;

    // ORDER BY
    struct OrderCol {
        int col_idx = 0;
        bool descending = true;
    };
    std::vector<OrderCol> order_cols;

    // LIMIT
    bool use_limit = true;
    int limit_value = 50;

    void reset();
    std::string build_sql(const char* const* columns, int num_columns) const;
};

class StatsPanel {
public:
    void render(const TraceModel& model, QueryDb& db, ViewState& view);
    void set_window(SDL_Window* window) { window_ = window; }

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
    float sql_height_ = 0.0f;  // draggable SQL editor height (0 = use default)

    SDL_Window* window_ = nullptr;

    bool show_schema_ = false;
    bool show_builder_ = false;
    QueryBuilderState builder_;

    void ensure_default_tab();
    void render_tab(QueryTab& tab, const TraceModel& model, QueryDb& db, ViewState& view);
    void render_schema_popup(QueryDb& db);
    void render_builder_popup(QueryDb& db);
};
