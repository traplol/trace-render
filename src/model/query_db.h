#pragma once
#include "trace_model.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct sqlite3;

class QueryDb {
public:
    QueryDb();
    ~QueryDb();

    // Populate the database from a trace model
    void load(const TraceModel& model);

    struct QueryResult {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        std::string error;
        bool ok = false;
    };

    // Synchronous execute (used internally)
    QueryResult execute(const std::string& sql);

    // Async query support
    void execute_async(const std::string& sql);
    bool is_query_running() const { return query_running_.load(std::memory_order_relaxed); }
    bool is_query_done() const { return query_done_.load(std::memory_order_acquire); }
    void cancel_query();
    QueryResult take_result();
    int query_rows_so_far() const { return query_rows_.load(std::memory_order_relaxed); }
    uint64_t query_steps() const { return query_steps_.load(std::memory_order_relaxed); }

    bool is_loaded() const { return loaded_; }

private:
    sqlite3* db_ = nullptr;
    bool loaded_ = false;

    // Async query state
    std::thread query_thread_;
    std::atomic<bool> query_running_{false};
    std::atomic<bool> query_done_{false};
    std::atomic<bool> query_cancel_{false};
    std::atomic<int> query_rows_{0};
    std::atomic<uint64_t> query_steps_{0};
    QueryResult async_result_;
    std::mutex result_mutex_;

    static int progress_callback(void* data);
};
