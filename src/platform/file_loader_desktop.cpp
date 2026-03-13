#include "file_loader.h"
#include "parser/trace_parser.h"
#include "tracing.h"
#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>

struct FileLoader::Impl {
    std::thread thread;
    std::atomic<bool> loading{false};
    std::atomic<bool> finished{false};
    std::atomic<float> progress{0.0f};
    std::atomic<float> phase_progress{0.0f};
    std::mutex phase_mutex;
    std::string phase_str;
    bool success_ = false;
    std::string error_;
    std::string filename_;
    TraceModel model;

    void setup_progress(TraceParser& parser) {
        parser.on_progress = [this](const char* ph, float p) {
            {
                std::lock_guard<std::mutex> lock(phase_mutex);
                phase_str = ph;
            }
            phase_progress.store(p, std::memory_order_relaxed);

            float global = 0.0f;
            if (std::string_view(ph) == "Reading file") {
                global = p * 0.25f;
            } else if (std::string_view(ph) == "Parsing JSON") {
                global = 0.25f + p * 0.35f;
            } else if (std::string_view(ph) == "Building index") {
                global = 0.60f + p * 0.20f;
            } else {
                global = 0.80f + p * 0.20f;
            }
            progress.store(global, std::memory_order_relaxed);
        };
    }

    void finish_parse(bool ok, TraceParser& parser, TraceModel& new_model, QueryDb* query_db) {
        if (ok) {
            model = std::move(new_model);
            if (query_db) {
                {
                    std::lock_guard<std::mutex> lock(phase_mutex);
                    phase_str = "Building query DB";
                }
                phase_progress.store(0.0f, std::memory_order_relaxed);
                progress.store(0.90f, std::memory_order_relaxed);
                query_db->load(model);
            }
            success_ = true;
        } else {
            success_ = false;
            error_ = parser.error_message;
        }
        finished.store(true, std::memory_order_release);
    }
};

FileLoader::FileLoader() : impl_(std::make_unique<Impl>()) {}
FileLoader::~FileLoader() {
    join();
}

void FileLoader::load_file(const std::string& path, bool time_ns, QueryDb* query_db) {
    join();

    impl_->filename_ = path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) impl_->filename_ = path.substr(pos + 1);

    impl_->loading = true;
    impl_->finished = false;
    impl_->progress = 0.0f;
    impl_->phase_progress = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    impl_->thread = std::thread([this, path, time_ns, query_db]() {
        TRACE_SCOPE_CAT("OpenFile", "io");

        TraceParser parser;
        impl_->setup_progress(parser);
        parser.time_unit_ns = time_ns;

        TraceModel new_model;
        bool ok = parser.parse(path, new_model);
        impl_->finish_parse(ok, parser, new_model, query_db);
    });
}

void FileLoader::load_buffer(std::vector<char> data, const std::string& filename, bool time_ns, QueryDb* query_db) {
    join();

    impl_->filename_ = filename;
    impl_->loading = true;
    impl_->finished = false;
    impl_->progress = 0.0f;
    impl_->phase_progress = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    impl_->thread = std::thread([this, data = std::move(data), time_ns, query_db]() {
        TRACE_SCOPE_CAT("OpenBuffer", "io");

        TraceParser parser;
        impl_->setup_progress(parser);
        parser.time_unit_ns = time_ns;

        TraceModel new_model;
        bool ok = parser.parse_buffer(data.data(), data.size(), new_model);
        impl_->finish_parse(ok, parser, new_model, query_db);
    });
}

bool FileLoader::is_loading() const {
    return impl_->loading.load(std::memory_order_relaxed);
}

bool FileLoader::poll_finished() {
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
    return impl_->progress.load(std::memory_order_relaxed);
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
