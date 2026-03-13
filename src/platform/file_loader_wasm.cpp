#include "file_loader.h"
#include "parser/trace_parser.h"
#include "tracing.h"
#include <string_view>

struct FileLoader::Impl {
    bool loading = false;
    bool finished = false;
    float progress_ = 0.0f;
    float phase_progress_ = 0.0f;
    std::string phase_str;
    bool success_ = false;
    std::string error_;
    std::string filename_;
    TraceModel model;

    void setup_progress(TraceParser& parser) {
        parser.on_progress = [this](const char* ph, float p) {
            phase_str = ph;
            phase_progress_ = p;

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
            progress_ = global;
        };
    }
};

FileLoader::FileLoader() : impl_(std::make_unique<Impl>()) {}
FileLoader::~FileLoader() = default;

void FileLoader::load_file(const std::string& path, bool time_ns) {
    impl_->filename_ = path;
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) impl_->filename_ = path.substr(pos + 1);

    impl_->loading = true;
    impl_->finished = false;
    impl_->progress_ = 0.0f;
    impl_->phase_progress_ = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    TraceParser parser;
    parser.time_unit_ns = time_ns;
    impl_->setup_progress(parser);

    TRACE_SCOPE_CAT("OpenFile", "io");
    TraceModel new_model;
    bool ok = parser.parse(path, new_model);

    if (ok) {
        impl_->model = std::move(new_model);
        impl_->success_ = true;
    } else {
        impl_->success_ = false;
        impl_->error_ = parser.error_message;
    }
    impl_->finished = true;
}

void FileLoader::load_buffer(const char* data, size_t size, const std::string& filename, bool time_ns) {
    impl_->filename_ = filename;

    impl_->loading = true;
    impl_->finished = false;
    impl_->progress_ = 0.0f;
    impl_->phase_progress_ = 0.0f;
    impl_->success_ = false;
    impl_->error_.clear();

    TraceParser parser;
    parser.time_unit_ns = time_ns;
    impl_->setup_progress(parser);

    TRACE_SCOPE_CAT("OpenBuffer", "io");
    TraceModel new_model;
    bool ok = parser.parse_buffer(data, size, new_model);

    if (ok) {
        impl_->model = std::move(new_model);
        impl_->success_ = true;
    } else {
        impl_->success_ = false;
        impl_->error_ = parser.error_message;
    }
    impl_->finished = true;
}

bool FileLoader::is_loading() const {
    return impl_->loading;
}

bool FileLoader::poll_finished() {
    if (!impl_->finished) return false;
    impl_->finished = false;
    impl_->loading = false;
    return true;
}

void FileLoader::join() {
    // No-op: no threads
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
    return impl_->progress_;
}

float FileLoader::phase_progress() const {
    return impl_->phase_progress_;
}

std::string FileLoader::phase() const {
    return impl_->phase_str;
}

TraceModel FileLoader::take_model() {
    return std::move(impl_->model);
}
