#pragma once
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

class Tracer {
public:
    static Tracer& instance() {
        static Tracer t;
        return t;
    }

    void set_output(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) close_file();
        file_ = fopen(path.c_str(), "w");
        if (file_) {
            fprintf(file_, "{\"traceEvents\":[\n");
            first_event_ = true;
            enabled_ = true;
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_file();
    }

    bool enabled() const { return enabled_; }

    void write_complete(const char* name, const char* cat,
                        uint64_t ts_us, uint64_t dur_us) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return;

        uint32_t tid = get_tid();

        if (!first_event_) fprintf(file_, ",\n");
        first_event_ = false;

        fprintf(file_,
            "{\"ph\":\"X\",\"name\":\"%s\",\"cat\":\"%s\","
            "\"ts\":%llu,\"dur\":%llu,"
            "\"pid\":1,\"tid\":%u}",
            name, cat, (unsigned long long)ts_us, (unsigned long long)dur_us, tid);
    }

    void write_counter(const char* name, const char* cat,
                       uint64_t ts_us, const char* key, double value) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return;

        uint32_t tid = get_tid();

        if (!first_event_) fprintf(file_, ",\n");
        first_event_ = false;

        fprintf(file_,
            "{\"ph\":\"C\",\"name\":\"%s\",\"cat\":\"%s\","
            "\"ts\":%llu,"
            "\"pid\":1,\"tid\":%u,"
            "\"args\":{\"%s\":%.2f}}",
            name, cat, (unsigned long long)ts_us, tid, key, value);
    }

    uint64_t now_us() const {
        auto now = std::chrono::steady_clock::now();
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            now - epoch_).count();
    }

private:
    Tracer() : epoch_(std::chrono::steady_clock::now()) {}
    ~Tracer() { close_file(); }

    void close_file() {
        if (file_) {
            fprintf(file_, "\n],\n");
            // Write metadata for thread names
            fprintf(file_, "\"displayTimeUnit\":\"us\"\n}\n");
            fclose(file_);
            file_ = nullptr;
            enabled_ = false;
        }
    }

    uint32_t get_tid() {
        // Simple hash of thread id to a small number
        auto id = std::this_thread::get_id();
        std::hash<std::thread::id> hasher;
        return (uint32_t)(hasher(id) & 0xFFFF);
    }

    FILE* file_ = nullptr;
    bool first_event_ = true;
    bool enabled_ = false;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point epoch_;
};

// RAII scope tracer
class TraceScope {
public:
    TraceScope(const char* name, const char* cat = "app")
        : name_(name), cat_(cat) {
        if (Tracer::instance().enabled()) {
            start_ = Tracer::instance().now_us();
            active_ = true;
        }
    }
    ~TraceScope() {
        if (active_) {
            uint64_t end = Tracer::instance().now_us();
            Tracer::instance().write_complete(name_, cat_, start_, end - start_);
        }
    }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    const char* name_;
    const char* cat_;
    uint64_t start_ = 0;
    bool active_ = false;
};

#define TRACE_SCOPE(name) TraceScope _trace_scope_##__LINE__(name)
#define TRACE_SCOPE_CAT(name, cat) TraceScope _trace_scope_##__LINE__(name, cat)
