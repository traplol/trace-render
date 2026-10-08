#include "trace_parser.h"
#include "tracing.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cmath>

using json = nlohmann::json;

struct SaxHandler : json::json_sax_t {
    TraceModel& model;
    std::function<void(const char*, float)>& on_progress;
    double time_divisor = 1.0;
    size_t file_size = 0;
    size_t bytes_read = 0;

    enum class State {
        TopLevel,
        InTopObject,
        InTraceEvents,
        InEvent,
        InArgs,
        InStackFrames,
        InStackFrame,
        Skipping,
    };

    State state = State::TopLevel;
    State skip_return_state = State::TopLevel;
    int skip_depth = 0;
    std::string current_key;
    StackFrame current_frame{};
    bool frame_has_name = false;

    // Current event being assembled
    TraceEvent current_event{};
    std::string current_args_json;
    int args_depth = 0;
    bool first_args_key = true;

    // Counter args accumulation
    std::vector<std::pair<std::string, double>> counter_values;

    uint64_t event_count = 0;
    uint64_t estimated_events = 0;

    SaxHandler(TraceModel& m, std::function<void(const char*, float)>& prog) : model(m), on_progress(prog) {}

    void skip_value() {
        skip_return_state = state;
        state = State::Skipping;
        skip_depth = 1;
    }

    void finish_event() {
        if (current_event.ph == Phase::Sample) {
            current_event.kind = EventKind::Sample;
            current_event.dur = 0;
            if (current_event.sample_weight >= 0) {
                const auto& unit = model.get_string(current_event.sample_weight_unit);
                double scale = unit == "us"   ? 1
                               : unit == "ns" ? 0.001
                               : unit == "ms" ? 1000
                               : unit == "s"  ? 1000000
                                              : -1;
                double cpu = current_event.sample_weight * scale;
                if (scale > 0 && std::isfinite(cpu)) current_event.sample_cpu_time = cpu;
            }
        } else if ((current_event.ph == Phase::DurationBegin || current_event.ph == Phase::DurationEnd ||
                    current_event.ph == Phase::Complete) &&
                   ("," + model.get_string(current_event.cat_idx) + ",").find(",sampleEvent,") != std::string::npos) {
            current_event.kind = EventKind::SampledSpan;
        }
        event_count++;
        if (on_progress && (event_count & 0xFFFF) == 0 && estimated_events > 0) {
            float p = std::min(0.99f, (float)event_count / (float)estimated_events);
            on_progress("Parsing JSON", p);
        }

        if (current_event.ph == Phase::Metadata) {
            handle_metadata();
            return;
        }

        uint32_t ev_idx = (uint32_t)model.events().size();

        // Store args if present
        if (!current_args_json.empty()) {
            current_event.args_idx = model.add_args(std::move(current_args_json));
            current_args_json.clear();
        }

        // Handle counter events
        if (current_event.ph == Phase::Counter) {
            const std::string& event_name = model.get_string(current_event.name_idx);
            for (auto& [arg_key, val] : counter_values) {
                // Use event name; append arg key if multiple values
                std::string series_name = event_name;
                if (counter_values.size() > 1) {
                    series_name += "." + arg_key;
                }
                auto& cs = model.find_or_create_counter_series(current_event.pid, series_name);
                cs.points.push_back({current_event.ts, val});
            }
            counter_values.clear();
        }

        // Handle flow events
        if (current_event.ph == Phase::FlowStart || current_event.ph == Phase::FlowStep ||
            current_event.ph == Phase::FlowEnd) {
            model.add_flow_event(current_event.id, ev_idx);
        }

        model.add_event(current_event);

        // Duration/sample thread indexes are populated once by build_index().
        if (current_event.ph == Phase::Counter) model.get_or_create_process(current_event.pid);
    }

    void handle_metadata() {
        TRACE_FUNCTION_CAT("parser");
        const std::string& name = model.get_string(current_event.name_idx);
        auto& proc = model.get_or_create_process(current_event.pid);

        if (name == "process_name") {
            // Extract name from args
            if (current_event.args_idx != UINT32_MAX || !current_args_json.empty()) {
                const std::string& args_str =
                    current_args_json.empty() ? model.args()[current_event.args_idx] : current_args_json;
                try {
                    auto args = json::parse(args_str);
                    if (args.contains("name")) {
                        proc.name = args["name"].get<std::string>();
                    }
                } catch (...) {}
            }
        } else if (name == "thread_name") {
            auto& thread = proc.get_or_create_thread(current_event.tid);
            if (!current_args_json.empty()) {
                try {
                    auto args = json::parse(current_args_json);
                    if (args.contains("name")) {
                        thread.name = args["name"].get<std::string>();
                    }
                } catch (...) {}
            }
        } else if (name == "process_sort_index") {
            if (!current_args_json.empty()) {
                try {
                    auto args = json::parse(current_args_json);
                    if (args.contains("sort_index")) {
                        proc.sort_index = args["sort_index"].get<int32_t>();
                    }
                } catch (...) {}
            }
        } else if (name == "thread_sort_index") {
            auto& thread = proc.get_or_create_thread(current_event.tid);
            if (!current_args_json.empty()) {
                try {
                    auto args = json::parse(current_args_json);
                    if (args.contains("sort_index")) {
                        thread.sort_index = args["sort_index"].get<int32_t>();
                    }
                } catch (...) {}
            }
        }
        current_args_json.clear();
    }

    void args_append(const std::string& s) {
        if (!first_args_key) current_args_json += ",";
        current_args_json += s;
    }

    // --- SAX callbacks ---

    bool null() override {
        TRACE_FUNCTION_CAT("parser");
        if (state == State::InArgs) {
            args_append("null");
            return true;
        }
        if (state == State::Skipping) return true;
        if (state == State::InStackFrame && (current_key == "parent" || current_key == "name"))
            current_frame.valid = false;
        return true;
    }

    bool boolean(bool val) override {
        TRACE_FUNCTION_CAT("parser");
        if (state == State::InArgs) {
            args_append(val ? "true" : "false");
            return true;
        }
        if (state == State::Skipping) return true;
        if (state == State::InStackFrame && (current_key == "parent" || current_key == "name"))
            current_frame.valid = false;
        return true;
    }

    bool number_integer(number_integer_t val) override { return handle_number((double)val, std::to_string(val)); }

    bool number_unsigned(number_unsigned_t val) override { return handle_number((double)val, std::to_string(val)); }

    bool number_float(number_float_t val, const string_t& s) override {
        return handle_number(val, s.empty() ? std::to_string(val) : s);
    }

    bool handle_number(double val, const std::string& raw) {
        if (state == State::InArgs) {
            if (args_depth == 0) {
                // Top-level arg value - for counter events, capture the value
                if (current_event.ph == Phase::Counter) {
                    counter_values.push_back({current_key, val});
                }
                args_append("\"" + current_key + "\":" + raw);
                first_args_key = false;
            } else {
                current_args_json += raw;
            }
            return true;
        }
        if (state == State::Skipping) return true;
        if (state == State::InStackFrame) {
            if (current_key == "parent")
                current_frame.parent_id = model.intern_string(raw);
            else if (current_key == "name")
                current_frame.valid = false;
            return true;
        }
        if (state == State::InEvent) {
            if (current_key == "ts")
                current_event.ts = val / time_divisor;
            else if (current_key == "dur")
                current_event.dur = val / time_divisor;
            else if (current_key == "pid")
                current_event.pid = (uint32_t)val;
            else if (current_key == "tid")
                current_event.tid = (uint32_t)val;
            else if (current_key == "id")
                current_event.id = (uint64_t)val;
            else if (current_key == "sf")
                current_event.stack_frame_id = model.intern_string(raw);
            else if (current_key == "weight" && std::isfinite(val) && val >= 0)
                current_event.sample_weight = val;
        }
        return true;
    }

    bool string(string_t& val) override {
        if (state == State::InArgs) {
            if (args_depth == 0) {
                std::string escaped = escape_json_string(val);
                args_append("\"" + current_key + "\":\"" + escaped + "\"");
                first_args_key = false;
                if (current_event.ph == Phase::Counter) {
                    // Try parsing string as number for counter
                    try {
                        counter_values.push_back({current_key, std::stod(val)});
                    } catch (...) {}
                }
            } else {
                current_args_json += "\"" + escape_json_string(val) + "\"";
            }
            return true;
        }
        if (state == State::Skipping) return true;
        if (state == State::InStackFrame) {
            if (current_key == "name") {
                current_frame.name_idx = model.intern_string(val);
                frame_has_name = true;
            } else if (current_key == "category")
                current_frame.cat_idx = model.intern_string(val);
            else if (current_key == "parent")
                current_frame.parent_id = model.intern_string(val);
            return true;
        }
        if (state == State::InEvent) {
            if (current_key == "name") {
                current_event.name_idx = model.intern_string(val);
            } else if (current_key == "cat") {
                current_event.cat_idx = model.intern_string(val);
            } else if (current_key == "ph") {
                if (!val.empty()) current_event.ph = phase_from_char(val[0]);
            } else if (current_key == "sf") {
                current_event.stack_frame_id = model.intern_string(val);
            } else if (current_key == "weightUnit") {
                current_event.sample_weight_unit = model.intern_string(val);
            } else if (current_key == "id") {
                // Hex string id like "0x1234"
                if (val.size() > 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
                    current_event.id = std::stoull(val, nullptr, 16);
                } else {
                    try {
                        current_event.id = std::stoull(val);
                    } catch (...) {}
                }
            }
        }
        if (state == State::InTopObject) {
            // Could be metadata values at top level
        }
        return true;
    }

    bool binary(binary_t&) override { return true; }

    bool start_object(std::size_t) override {
        if (state == State::Skipping) {
            skip_depth++;
            return true;
        }
        if (state == State::TopLevel) {
            state = State::InTopObject;
            return true;
        }
        if (state == State::InTopObject && current_key == "stackFrames") {
            state = State::InStackFrames;
            return true;
        }
        if (state == State::InStackFrames) {
            current_frame = StackFrame{};
            current_frame.id_idx = model.intern_string(current_key);
            frame_has_name = false;
            state = State::InStackFrame;
            return true;
        }
        if (state == State::InTraceEvents) {
            state = State::InEvent;
            current_event = TraceEvent{};
            current_args_json.clear();
            counter_values.clear();
            return true;
        }
        if (state == State::InEvent && current_key == "args") {
            state = State::InArgs;
            args_depth = 0;
            first_args_key = true;
            current_args_json = "{";
            return true;
        }
        if (state == State::InArgs) {
            args_depth++;
            current_args_json += "{";
            return true;
        }
        if (state == State::InEvent || state == State::InTopObject || state == State::InStackFrame) {
            if (state == State::InStackFrame && (current_key == "parent" || current_key == "name"))
                current_frame.valid = false;
            skip_value();
            return true;
        }
        return true;
    }

    bool end_object() override {
        if (state == State::Skipping) {
            skip_depth--;
            if (skip_depth == 0) state = skip_return_state;
            return true;
        }
        if (state == State::InStackFrame) {
            current_frame.valid = current_frame.valid && frame_has_name;
            model.add_stack_frame(current_frame);
            state = State::InStackFrames;
            return true;
        }
        if (state == State::InStackFrames) {
            state = State::InTopObject;
            return true;
        }
        if (state == State::InArgs) {
            if (args_depth > 0) {
                args_depth--;
                current_args_json += "}";
            } else {
                current_args_json += "}";
                state = State::InEvent;
            }
            return true;
        }
        if (state == State::InEvent) {
            finish_event();
            state = State::InTraceEvents;
            return true;
        }
        if (state == State::InTopObject) {
            state = State::TopLevel;
            return true;
        }
        return true;
    }

    bool start_array(std::size_t) override {
        TRACE_FUNCTION_CAT("parser");
        if (state == State::Skipping) {
            skip_depth++;
            return true;
        }
        if (state == State::TopLevel) {
            state = State::InTraceEvents;
            return true;
        }
        if (state == State::InTopObject && current_key == "traceEvents") {
            state = State::InTraceEvents;
            return true;
        }
        if (state == State::InArgs) {
            args_depth++;
            current_args_json += "[";
            return true;
        }
        if (state == State::InEvent || state == State::InTopObject || state == State::InStackFrame ||
            state == State::InStackFrames) {
            if (state == State::InStackFrame && (current_key == "parent" || current_key == "name"))
                current_frame.valid = false;
            skip_value();
            return true;
        }
        return true;
    }

    bool end_array() override {
        TRACE_FUNCTION_CAT("parser");
        if (state == State::Skipping) {
            skip_depth--;
            if (skip_depth == 0) state = skip_return_state;
            return true;
        }
        if (state == State::InArgs) {
            if (args_depth > 0) {
                args_depth--;
                current_args_json += "]";
            }
            return true;
        }
        if (state == State::InTraceEvents) {
            state = State::InTopObject;  // might have more top-level keys
            return true;
        }
        return true;
    }

    bool key(string_t& val) override {
        if (state == State::Skipping) return true;
        if (state == State::InArgs && args_depth > 0) {
            current_args_json += "\"" + escape_json_string(val) + "\":";
            return true;
        }
        current_key = val;
        if (state == State::InArgs && args_depth == 0) {
            // Will be handled by the value callback
        }
        return true;
    }

    bool parse_error(std::size_t position, const std::string& last_token, const json::exception& ex) override {
        TRACE_FUNCTION_CAT("parser");
        (void)position;
        (void)last_token;
        (void)ex;
        return false;
    }

    static std::string escape_json_string(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                        result += buf;
                    } else {
                        result += c;
                    }
                    break;
            }
        }
        return result;
    }
};

bool TraceParser::parse(const std::string& filepath, TraceModel& model) {
    TRACE_FUNCTION_CAT("parser");
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        error_message_ = "Could not open file: " + filepath;
        return false;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (on_progress_) on_progress_("Reading file", 0.0f);

    // Read file in chunks to report progress
    std::string content(file_size, '\0');
    {
        constexpr size_t CHUNK = 4 * 1024 * 1024;  // 4MB chunks
        size_t read_so_far = 0;
        while (read_so_far < file_size) {
            size_t to_read = std::min(CHUNK, file_size - read_so_far);
            file.read(&content[read_so_far], to_read);
            read_so_far += to_read;
            if (on_progress_) {
                on_progress_("Reading file", (float)read_so_far / (float)file_size);
            }
        }
        file.close();
    }

    model.clear();
    // Intern empty string at index 0
    model.intern_string("");

    if (on_progress_) on_progress_("Parsing JSON", 0.0f);

    SaxHandler handler(model, on_progress_);
    handler.time_divisor = time_unit_ns_ ? 1000.0 : 1.0;
    handler.file_size = file_size;
    // Rough estimate: ~100 bytes per event in JSON
    handler.estimated_events = file_size / 100;

    bool result;
    { result = json::sax_parse(content, &handler); }

    // Free the raw JSON string before building the index
    content.clear();
    content.shrink_to_fit();

    if (!result) {
        error_message_ = "JSON parse error";
        return false;
    }

    if (on_progress_) on_progress_("Building index", 0.0f);
    model.build_index(on_progress_ ? [this](float p) { on_progress_("Building index", p); }
                                   : std::function<void(float)>{});

    if (on_progress_) on_progress_("Done", 1.0f);

    return true;
}

bool TraceParser::parse_buffer(const char* data, size_t size, TraceModel& model) {
    TRACE_FUNCTION_CAT("parser");

    model.clear();
    model.intern_string("");

    if (on_progress_) on_progress_("Parsing JSON", 0.0f);

    SaxHandler handler(model, on_progress_);
    handler.time_divisor = time_unit_ns_ ? 1000.0 : 1.0;
    handler.file_size = size;
    handler.estimated_events = size / 100;

    std::string_view sv(data, size);
    bool result = json::sax_parse(sv, &handler);

    if (!result) {
        error_message_ = "JSON parse error";
        return false;
    }

    if (on_progress_) on_progress_("Building index", 0.0f);
    model.build_index(on_progress_ ? [this](float p) { on_progress_("Building index", p); }
                                   : std::function<void(float)>{});

    if (on_progress_) on_progress_("Done", 1.0f);

    return true;
}
