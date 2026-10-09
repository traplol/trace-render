#include "file_loader.h"
#include "parser/trace_parser.h"
#include "symbols/native_symbol_resolver.h"
#include "tracing.h"
#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>
#include <stdexcept>

struct FileLoader::Impl {
    std::thread thread;
    std::atomic<bool> loading{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> cancelled{false};
    std::atomic<float> phase_progress{0.0f};
    std::mutex phase_mutex;
    std::string phase_str;
    bool success_ = false;
    std::string error_;
    std::string filename_;
    TraceModel model;
    std::vector<std::string> symbol_paths;

    void report_progress(const char* ph, float p) {
        if (cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Import canceled");
        {
            std::lock_guard<std::mutex> lock(phase_mutex);
            phase_str = ph;
        }
        phase_progress.store(p, std::memory_order_relaxed);
        // Show current phase progress; different resources may start a new phase.
    }

    void fail(const std::string& message) {
        model.clear();
        success_ = false;
        error_ = message;
        finished.store(true, std::memory_order_release);
    }

    void setup_progress(TraceParser& parser) {
        parser.set_on_progress([this](const char* ph, float p) { report_progress(ph, p); });
    }

    void finish_parse(bool ok, TraceParser& parser, TraceModel& new_model, QueryDb* query_db,
                      NativeSymbolResolver& resolver) {
        if (cancelled.load(std::memory_order_relaxed)) {
            fail("Import canceled");
            return;
        }
        if (ok) {
            if (!new_model.profile().modules.empty()) {
                resolver.resolve_profile(new_model, [this](float p) {
                    report_progress("Resolving native symbols", p);
                    return true;
                });
            }
            model = std::move(new_model);
            if (query_db) {
                {
                    std::lock_guard<std::mutex> lock(phase_mutex);
                    phase_str = "Building query DB";
                }
                phase_progress.store(0.0f, std::memory_order_relaxed);
                query_db->load(model, [this](float p) { phase_progress.store(p, std::memory_order_relaxed); });
            }
            if (cancelled.load(std::memory_order_relaxed)) {
                fail("Import canceled");
                return;
            }
            success_ = true;
        } else {
            fail(parser.error_message());
            return;
        }
        finished.store(true, std::memory_order_release);
    }
};

FileLoader::FileLoader() : impl_(std::make_unique<Impl>()) {}
FileLoader::~FileLoader() {
    cancel();
    join();
}

void FileLoader::cancel() {
    impl_->cancelled.store(true, std::memory_order_relaxed);
}
void FileLoader::set_symbol_paths(std::vector<std::string> paths) {
    impl_->symbol_paths = std::move(paths);
}

void FileLoader::load_file(const std::string& path, bool time_ns, QueryDb* query_db) {
    TRACE_FUNCTION_CAT("platform");
    join();

    impl_->filename_ = path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) impl_->filename_ = path.substr(pos + 1);

    impl_->loading = true;
    impl_->finished = false;
    impl_->cancelled = false;
    impl_->phase_progress = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    impl_->thread = std::thread([this, path, time_ns, query_db, paths = impl_->symbol_paths]() {
        try {
            NativeSymbolResolver resolver(paths);
            TraceParser parser;
            parser.set_native_symbols(&resolver);
            impl_->setup_progress(parser);
            parser.set_time_unit_ns(time_ns);

            TraceModel new_model;
            bool ok = parser.parse(path, new_model);
            impl_->finish_parse(ok, parser, new_model, query_db, resolver);
        } catch (const std::exception& error) {
            impl_->fail(error.what());
        }
    });
}

void FileLoader::load_buffer(std::vector<char> data, const std::string& filename, bool time_ns, QueryDb* query_db) {
    join();

    impl_->filename_ = filename;
    impl_->loading = true;
    impl_->finished = false;
    impl_->cancelled = false;
    impl_->phase_progress = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    impl_->thread = std::thread([this, data = std::move(data), time_ns, query_db, paths = impl_->symbol_paths]() {
        try {
            NativeSymbolResolver resolver(paths);
            TraceParser parser;
            parser.set_native_symbols(&resolver);
            impl_->setup_progress(parser);
            parser.set_time_unit_ns(time_ns);

            TraceModel new_model;
            bool ok = parser.parse_buffer(data.data(), data.size(), new_model);
            impl_->finish_parse(ok, parser, new_model, query_db, resolver);
        } catch (const std::exception& error) {
            impl_->fail(error.what());
        }
    });
}

bool FileLoader::is_loading() const {
    return impl_->loading.load(std::memory_order_relaxed);
}

bool FileLoader::poll_finished() {
    TRACE_FUNCTION_CAT("platform");
    if (!impl_->finished.load(std::memory_order_acquire)) return false;
    join();
    impl_->loading = false;
    impl_->finished = false;
    return true;
}

void FileLoader::join() {
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool FileLoader::success() const {
    return impl_->success_;
}

const std::string& FileLoader::error() const {
    return impl_->error_;
}

const std::string& FileLoader::filename() const {
    return impl_->filename_;
}

float FileLoader::progress() const {
    return impl_->phase_progress.load(std::memory_order_relaxed);
}

float FileLoader::phase_progress() const {
    return impl_->phase_progress.load(std::memory_order_relaxed);
}

std::string FileLoader::phase() const {
    std::lock_guard<std::mutex> lock(impl_->phase_mutex);
    return impl_->phase_str;
}

TraceModel FileLoader::take_model() {
    return std::move(impl_->model);
}
