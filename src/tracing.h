#pragma once
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

// --- JSON arg serialization helpers ---
namespace trace_detail {

inline void escape_json_string(std::string& out, const char* s) {
    out += '"';
    for (; *s; ++s) {
        switch (*s) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(*s) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*s);
                    out += buf;
                } else {
                    out += *s;
                }
                break;
        }
    }
    out += '"';
}

// Value serialization overloads
inline void append_arg_value(std::string& out, const char* v) {
    if (!v) {
        out += "null";
        return;
    }
    escape_json_string(out, v);
}
inline void append_arg_value(std::string& out, const std::string& v) {
    escape_json_string(out, v.c_str());
}
inline void append_arg_value(std::string& out, bool v) {
    out += v ? "true" : "false";
}
inline void append_arg_value(std::string& out, int v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, long v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, long long v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, unsigned int v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, unsigned long v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, unsigned long long v) {
    out += std::to_string(v);
}
inline void append_arg_value(std::string& out, float v) {
    if (std::isnan(v) || std::isinf(v)) {
        out += "null";
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", (double)v);
    out += buf;
}
inline void append_arg_value(std::string& out, double v) {
    if (std::isnan(v) || std::isinf(v)) {
        out += "null";
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", v);
    out += buf;
}

// Base case: no more pairs
inline void append_args(std::string&) {}

// Recursive: consume key-value pairs
template <typename V, typename... Rest>
void append_args(std::string& out, const char* key, const V& val, Rest&&... rest) {
    if (!out.empty()) out += ',';
    escape_json_string(out, key);
    out += ':';
    append_arg_value(out, val);
    append_args(out, std::forward<Rest>(rest)...);
}

// Entry point: returns the inner JSON (without outer braces)
// Returns empty string for zero args
template <typename... Args>
std::string make_args_json(Args&&... args) {
    std::string out;
    append_args(out, std::forward<Args>(args)...);
    return out;
}

}  // namespace trace_detail

// Portable qualified function name
#if defined(_MSC_VER)
#define TRACE_FUNC_SIG __FUNCSIG__
#else
#define TRACE_FUNC_SIG __PRETTY_FUNCTION__
#endif

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

    void write_complete(const char* name, const char* cat, uint64_t ts_us, uint64_t dur_us,
                        const char* args_json = nullptr) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_) return;

        uint32_t tid = get_tid();

        if (!first_event_) fprintf(file_, ",\n");
        first_event_ = false;

        fprintf(file_,
                "{\"ph\":\"X\",\"name\":\"%s\",\"cat\":\"%s\","
                "\"ts\":%llu,\"dur\":%llu,"
                "\"pid\":1,\"tid\":%u",
                name, cat, (unsigned long long)ts_us, (unsigned long long)dur_us, tid);

        if (args_json && args_json[0]) {
            fprintf(file_, ",\"args\":{%s}", args_json);
        }

        fprintf(file_, "}");
    }

    void write_counter(const char* name, const char* cat, uint64_t ts_us, const char* key, double value) {
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
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
    }

private:
    Tracer() : epoch_(std::chrono::steady_clock::now()) {}
    ~Tracer() { close_file(); }

    void close_file() {
        if (file_) {
            fprintf(file_, "\n],\n");
            fprintf(file_, "\"displayTimeUnit\":\"us\"\n}\n");
            fclose(file_);
            file_ = nullptr;
            enabled_ = false;
        }
    }

    uint32_t get_tid() {
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

// RAII scope tracer — always emits file/line/func as args
class TraceScope {
public:
    TraceScope(const char* name, const char* cat, const char* file, int line, const char* func)
        : name_(name), cat_(cat), file_(file), line_(line), func_(func) {
        if (Tracer::instance().enabled()) {
            start_ = Tracer::instance().now_us();
            active_ = true;
        }
    }
    ~TraceScope() {
        if (active_) {
            uint64_t end = Tracer::instance().now_us();
            auto args = trace_detail::make_args_json("file", file_, "line", line_, "func", func_);
            Tracer::instance().write_complete(name_, cat_, start_, end - start_, args.c_str());
        }
    }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    const char* name_;
    const char* cat_;
    const char* file_;
    int line_;
    const char* func_;
    uint64_t start_ = 0;
    bool active_ = false;
};

// RAII scope tracer with extra args (merges location + user-supplied args)
class TraceScopeArgs {
public:
    TraceScopeArgs(const char* name, const char* cat, const char* file, int line, const char* func,
                   std::string extra_args)
        : name_(name), cat_(cat), file_(file), line_(line), func_(func) {
        if (Tracer::instance().enabled()) {
            extra_args_ = std::move(extra_args);
            start_ = Tracer::instance().now_us();
            active_ = true;
        }
    }
    ~TraceScopeArgs() {
        if (active_) {
            uint64_t end = Tracer::instance().now_us();
            auto args = trace_detail::make_args_json("file", file_, "line", line_, "func", func_);
            if (!extra_args_.empty()) {
                args += ',';
                args += extra_args_;
            }
            Tracer::instance().write_complete(name_, cat_, start_, end - start_, args.c_str());
        }
    }

    TraceScopeArgs(const TraceScopeArgs&) = delete;
    TraceScopeArgs& operator=(const TraceScopeArgs&) = delete;

private:
    const char* name_;
    const char* cat_;
    const char* file_;
    int line_;
    const char* func_;
    std::string extra_args_;
    uint64_t start_ = 0;
    bool active_ = false;
};

#define TRACE_SCOPE(name) \
    TraceScope _trace_scope_##__LINE__(name, "app", __FILE__, __LINE__, TRACE_FUNC_SIG)
#define TRACE_SCOPE_CAT(name, cat) \
    TraceScope _trace_scope_##__LINE__(name, cat, __FILE__, __LINE__, TRACE_FUNC_SIG)
#define TRACE_SCOPE_ARGS(name, cat, ...)                                         \
    TraceScopeArgs _trace_scope_args_##__LINE__(                                 \
        name, cat, __FILE__, __LINE__, TRACE_FUNC_SIG, \
        Tracer::instance().enabled() ? trace_detail::make_args_json(__VA_ARGS__) : std::string())
#define TRACE_FUNCTION()                                                                                    \
    TraceScope _trace_scope_##__LINE__(TRACE_FUNC_SIG, "app", __FILE__, __LINE__, \
                                       TRACE_FUNC_SIG)
#define TRACE_FUNCTION_CAT(cat)                                                                           \
    TraceScope _trace_scope_##__LINE__(TRACE_FUNC_SIG, cat, __FILE__, __LINE__, \
                                       TRACE_FUNC_SIG)
