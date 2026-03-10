#pragma once
#include "model/query_db.h"
#include "ui/view_state.h"
#include <string>
#include <vector>

class QueryPanel {
public:
    void render(QueryDb& db, ViewState& view);

private:
    char query_buf_[4096] = "SELECT name, COUNT(*) as count, SUM(dur) as total_dur, AVG(dur) as avg_dur\nFROM events\nWHERE dur > 0\nGROUP BY name\nORDER BY total_dur DESC\nLIMIT 50";
    QueryDb::QueryResult result_;
    bool has_result_ = false;
    std::string last_error_;
};
