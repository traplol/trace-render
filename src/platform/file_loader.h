#pragma once
#include "model/trace_model.h"
#include "model/query_db.h"
#include <memory>
#include <string>
#include <vector>

class FileLoader {
public:
    FileLoader();
    ~FileLoader();

    void load_file(const std::string& path, bool time_ns, QueryDb* query_db = nullptr);
    void load_buffer(std::vector<char> data, const std::string& filename, bool time_ns, QueryDb* query_db = nullptr);

    bool is_loading() const;
    bool poll_finished();  // returns true once when loading completes
    void join();           // block until done

    bool success() const;
    const std::string& error() const;
    const std::string& filename() const;
    float progress() const;
    float phase_progress() const;
    std::string phase() const;

    TraceModel take_model();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
