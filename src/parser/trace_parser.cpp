#include "trace_parser.h"
#include "tracing.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

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
        Skipping,
    };

    State state = State::TopLevel;
    int skip_depth = 0;
    std::string current_key;

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

    void finish_event() {
        event_count++;
        if (on_progress && (event_count & 0xFFFF) == 0 && estimated_events > 0) {
            float p = std::min(0.99f, (float)event_count / (float)estimated_events);
            on_progress("Parsing JSON", p);
        }

        if (current_event.ph == Phase::Metadata) {
            handle_metadata();
            return;
        }

        uint32_t ev_idx = (uint32_t)model.events_.size();

        // Store args if present
        if (!current_args_json.empty()) {
            current_event.args_idx = (uint32_t)model.args_.size();
            model.args_.push_back(std::move(current_args_json));
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
                // Find or create counter series
                CounterSeries* cs = nullptr;
                for (auto& s : model.counter_series_) {
                    if (s.pid == current_event.pid && s.name == series_name) {
                        cs = &s;
                        break;
                    }
                }
                if (!cs) {
                    model.counter_series_.push_back({});
                    cs = &model.counter_series_.back();
                    cs->pid = current_event.pid;
                    cs->name = series_name;
                }
                cs->points.push_back({current_event.ts, val});
            }
            counter_values.clear();
        }

        // Handle flow events
        if (current_event.ph == Phase::FlowStart || current_event.ph == Phase::FlowStep ||
            current_event.ph == Phase::FlowEnd) {
            model.flow_groups_[current_event.id].push_back(ev_idx);
        }

        model.events_.push_back(current_event);

        // Add to thread (skip counter events - they render as separate tracks)
        if (current_event.ph != Phase::Counter) {
            auto& proc = model.get_or_create_process(current_event.pid);
            auto& thread = proc.get_or_create_thread(current_event.tid);
            thread.event_indices.push_back(ev_idx);
        } else {
            // Ensure process exists for counter tracks
            model.get_or_create_process(current_event.pid);
        }
    }

    void handle_metadata() {
        const std::string& name = model.get_string(current_event.name_idx);
        auto& proc = model.get_or_create_process(current_event.pid);

        if (name == "process_name") {
            // Extract name from args
            if (current_event.args_idx != UINT32_MAX || !current_args_json.empty()) {
                const std::string& args_str =
                    current_args_json.empty() ? model.args_[current_event.args_idx] : current_args_json;
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
        if (state == State::InArgs) {
            args_append("null");
            return true;
        }
        if (state == State::Skipping) return true;
        return true;
    }

    bool boolean(bool val) override {
        if (state == State::InArgs) {
            args_append(val ? "true" : "false");
            return true;
        }
        if (state == State::Skipping) return true;
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
        if (state == State::InEvent) {
            if (current_key == "name") {
                current_event.name_idx = model.intern_string(val);
            } else if (current_key == "cat") {
                current_event.cat_idx = model.intern_string(val);
            } else if (current_key == "ph") {
                if (!val.empty()) current_event.ph = phase_from_char(val[0]);
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
        if (state == State::InEvent) {
            // Unknown object field - skip it
            state = State::Skipping;
            skip_depth = 1;
            return true;
        }
        return true;
    }

    bool end_object() override {
        if (state == State::Skipping) {
            skip_depth--;
            if (skip_depth == 0) state = State::InEvent;
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
        if (state == State::InEvent || state == State::InTopObject) {
            state = State::Skipping;
            skip_depth = 1;
            return true;
        }
        return true;
    }

    bool end_array() override {
        if (state == State::Skipping) {
            skip_depth--;
            if (skip_depth == 0) state = State::InEvent;
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
    TRACE_SCOPE_CAT("Parse", "io");
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        error_message = "Could not open file: " + filepath;
        return false;
    }

    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (on_progress) on_progress("Reading file", 0.0f);

    // Read file in chunks to report progress
    std::string content(file_size, '\0');
    constexpr size_t CHUNK = 4 * 1024 * 1024;  // 4MB chunks
    size_t read_so_far = 0;
    while (read_so_far < file_size) {
        size_t to_read = std::min(CHUNK, file_size - read_so_far);
        file.read(&content[read_so_far], to_read);
        read_so_far += to_read;
        if (on_progress) {
            on_progress("Reading file", (float)read_so_far / (float)file_size);
        }
    }
    file.close();

    model.clear();
    // Intern empty string at index 0
    model.intern_string("");

    if (on_progress) on_progress("Parsing JSON", 0.0f);

    SaxHandler handler(model, on_progress);
    handler.time_divisor = time_unit_ns ? 1000.0 : 1.0;
    handler.file_size = file_size;
    // Rough estimate: ~100 bytes per event in JSON
    handler.estimated_events = file_size / 100;

    bool result = json::sax_parse(content, &handler);

    // Free the raw JSON string before building the index
    content.clear();
    content.shrink_to_fit();

    if (!result) {
        error_message = "JSON parse error";
        return false;
    }

    if (on_progress) on_progress("Building index", 0.5f);
    model.build_index();

    if (on_progress) on_progress("Done", 1.0f);

    return true;
}
