#pragma once
#include "trace_model.h"
#include <string>
#include <vector>
#include <functional>

struct sqlite3;

class QueryDb {
public:
    QueryDb();
    ~QueryDb();

    // Populate the database from a trace model
    void load(const TraceModel& model);

    // Execute a SQL query; calls row_cb for each result row
    // Returns column names in out_columns, error message on failure
    struct QueryResult {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        std::string error;
        bool ok = false;
    };

    QueryResult execute(const std::string& sql);

    bool is_loaded() const { return loaded_; }

private:
    sqlite3* db_ = nullptr;
    bool loaded_ = false;
};
