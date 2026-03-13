#pragma once
#include "model/trace_model.h"
#include <memory>
#include <string>

class FileLoader {
public:
    FileLoader();
    ~FileLoader();

    void load_file(const std::string& path, bool time_ns);
    void load_buffer(const char* data, size_t size, const std::string& filename, bool time_ns);

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
