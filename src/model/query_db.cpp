#include "query_db.h"
#include <sqlite3.h>

QueryDb::QueryDb() {
    sqlite3_open(":memory:", &db_);
}

QueryDb::~QueryDb() {
    if (db_) sqlite3_close(db_);
}

void QueryDb::load(const TraceModel& model) {
    if (!db_) return;

    // Drop existing tables
    sqlite3_exec(db_, "DROP TABLE IF EXISTS events", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "DROP TABLE IF EXISTS processes", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "DROP TABLE IF EXISTS threads", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "DROP TABLE IF EXISTS counters", nullptr, nullptr, nullptr);

    // Create tables
    sqlite3_exec(db_, R"(
        CREATE TABLE events (
            id INTEGER PRIMARY KEY,
            name TEXT,
            category TEXT,
            phase TEXT,
            ts REAL,
            dur REAL,
            end_ts REAL,
            pid INTEGER,
            tid INTEGER,
            depth INTEGER
        )
    )", nullptr, nullptr, nullptr);

    sqlite3_exec(db_, R"(
        CREATE TABLE processes (
            pid INTEGER PRIMARY KEY,
            name TEXT
        )
    )", nullptr, nullptr, nullptr);

    sqlite3_exec(db_, R"(
        CREATE TABLE threads (
            tid INTEGER,
            pid INTEGER,
            name TEXT,
            PRIMARY KEY (pid, tid)
        )
    )", nullptr, nullptr, nullptr);

    sqlite3_exec(db_, R"(
        CREATE TABLE counters (
            pid INTEGER,
            name TEXT,
            ts REAL,
            value REAL
        )
    )", nullptr, nullptr, nullptr);

    // Insert data using transactions for speed
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);

    // Events
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO events VALUES (?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, nullptr);

    for (uint32_t i = 0; i < (uint32_t)model.events_.size(); i++) {
        const auto& ev = model.events_[i];
        if (ev.is_end_event) continue;

        const auto& name = model.get_string(ev.name_idx);
        const auto& cat = model.get_string(ev.cat_idx);

        char phase_str[2] = {(char)ev.ph, '\0'};

        sqlite3_bind_int(stmt, 1, i);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, cat.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, phase_str, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 5, ev.ts);
        sqlite3_bind_double(stmt, 6, ev.dur);
        sqlite3_bind_double(stmt, 7, ev.end_ts());
        sqlite3_bind_int(stmt, 8, ev.pid);
        sqlite3_bind_int(stmt, 9, ev.tid);
        sqlite3_bind_int(stmt, 10, ev.depth);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    // Processes
    sqlite3_prepare_v2(db_,
        "INSERT INTO processes VALUES (?,?)",
        -1, &stmt, nullptr);

    for (const auto& proc : model.processes_) {
        sqlite3_bind_int(stmt, 1, proc.pid);
        sqlite3_bind_text(stmt, 2, proc.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    // Threads
    sqlite3_prepare_v2(db_,
        "INSERT INTO threads VALUES (?,?,?)",
        -1, &stmt, nullptr);

    for (const auto& proc : model.processes_) {
        for (const auto& thread : proc.threads) {
            sqlite3_bind_int(stmt, 1, thread.tid);
            sqlite3_bind_int(stmt, 2, proc.pid);
            sqlite3_bind_text(stmt, 3, thread.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
    }
    sqlite3_finalize(stmt);

    // Counters
    sqlite3_prepare_v2(db_,
        "INSERT INTO counters VALUES (?,?,?,?)",
        -1, &stmt, nullptr);

    for (const auto& cs : model.counter_series_) {
        for (const auto& pt : cs.points) {
            sqlite3_bind_int(stmt, 1, cs.pid);
            sqlite3_bind_text(stmt, 2, cs.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, pt.first);
            sqlite3_bind_double(stmt, 4, pt.second);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
    }
    sqlite3_finalize(stmt);

    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

    // Create indexes
    sqlite3_exec(db_, "CREATE INDEX idx_events_name ON events(name)", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX idx_events_ts ON events(ts)", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX idx_events_pid_tid ON events(pid, tid)", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX idx_counters_name ON counters(name)", nullptr, nullptr, nullptr);

    loaded_ = true;
}

QueryDb::QueryResult QueryDb::execute(const std::string& sql) {
    QueryResult result;
    if (!db_ || !loaded_) {
        result.error = "No trace loaded";
        return result;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        result.error = sqlite3_errmsg(db_);
        return result;
    }

    int col_count = sqlite3_column_count(stmt);
    for (int i = 0; i < col_count; i++) {
        const char* name = sqlite3_column_name(stmt, i);
        result.columns.push_back(name ? name : "");
    }

    int row_limit = 10000;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && row_limit-- > 0) {
        std::vector<std::string> row;
        for (int i = 0; i < col_count; i++) {
            const char* val = (const char*)sqlite3_column_text(stmt, i);
            row.push_back(val ? val : "NULL");
        }
        result.rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    result.ok = true;

    if (row_limit <= 0) {
        result.error = "Results truncated to 10000 rows";
    }

    return result;
}
