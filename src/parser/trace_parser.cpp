#include "trace_parser.h"
#include "tracing.h"
#include <simdjson.h>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

static void parse_events(simdjson::ondemand::array events, TraceModel& model, double time_divisor,
                         std::function<void(const char*, float)>& on_progress, uint64_t estimated_events) {
    uint64_t event_count = 0;

    for (auto event_result : events) {
        auto obj = event_result.get_object().value();
        TraceEvent ev{};
        std::string_view args_raw;
        bool has_args = false;

        for (auto field : obj) {
            std::string_view key = field.unescaped_key().value();

            if (key == "name") {
                std::string_view val;
                if (!field.value().get_string().get(val)) ev.name_idx = model.intern_string(std::string(val));
            } else if (key == "cat") {
                std::string_view val;
                if (!field.value().get_string().get(val)) ev.cat_idx = model.intern_string(std::string(val));
            } else if (key == "ph") {
                std::string_view val;
                if (!field.value().get_string().get(val))
                    if (!val.empty()) ev.ph = phase_from_char(val[0]);
            } else if (key == "ts") {
                double val;
                if (!field.value().get_double().get(val)) ev.ts = val / time_divisor;
            } else if (key == "dur") {
                double val;
                if (!field.value().get_double().get(val)) ev.dur = val / time_divisor;
            } else if (key == "pid") {
                double val;
                if (!field.value().get_double().get(val)) ev.pid = (uint32_t)val;
            } else if (key == "tid") {
                double val;
                if (!field.value().get_double().get(val)) ev.tid = (uint32_t)val;
            } else if (key == "id") {
                try {
                    auto val = field.value();
                    auto t = val.type().value();
                    if (t == simdjson::ondemand::json_type::string) {
                        auto s = std::string(val.get_string().value());
                        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                            ev.id = std::stoull(s, nullptr, 16);
                        } else {
                            ev.id = std::stoull(s);
                        }
                    } else if (t == simdjson::ondemand::json_type::number) {
                        ev.id = (uint64_t)val.get_double().value();
                    }
                } catch (...) {}
            } else if (key == "args") {
                auto val = field.value();
                auto rj = val.raw_json();
                if (!rj.error()) {
                    args_raw = rj.value();
                    has_args = true;
                }
            }
            // Unknown fields are automatically skipped by simdjson
        }

        event_count++;
        if (on_progress && (event_count & 0xFFFF) == 0 && estimated_events > 0) {
            float p = std::min(0.99f, (float)event_count / (float)estimated_events);
            on_progress("Parsing JSON", p);
        }

        // Handle metadata events
        if (ev.ph == Phase::Metadata) {
            const std::string& name = model.get_string(ev.name_idx);
            auto& proc = model.get_or_create_process(ev.pid);
            if (has_args) {
                try {
                    auto args = json::parse(args_raw);
                    if (name == "process_name" && args.contains("name")) {
                        proc.name = args["name"].get<std::string>();
                    } else if (name == "thread_name" && args.contains("name")) {
                        proc.get_or_create_thread(ev.tid).name = args["name"].get<std::string>();
                    } else if (name == "process_sort_index" && args.contains("sort_index")) {
                        proc.sort_index = args["sort_index"].get<int32_t>();
                    } else if (name == "thread_sort_index" && args.contains("sort_index")) {
                        proc.get_or_create_thread(ev.tid).sort_index = args["sort_index"].get<int32_t>();
                    }
                } catch (...) {}
            }
            continue;
        }

        uint32_t ev_idx = (uint32_t)model.events().size();

        // Store args
        if (has_args) {
            ev.args_idx = model.add_args(std::string(args_raw));
        }

        // Handle counter events
        if (ev.ph == Phase::Counter && has_args) {
            const std::string& event_name = model.get_string(ev.name_idx);
            try {
                auto args = json::parse(args_raw);
                std::vector<std::pair<std::string, double>> counter_values;
                for (auto& [k, v] : args.items()) {
                    if (v.is_number()) {
                        counter_values.push_back({k, v.get<double>()});
                    } else if (v.is_string()) {
                        try {
                            counter_values.push_back({k, std::stod(v.get<std::string>())});
                        } catch (...) {}
                    }
                }
                for (auto& [arg_key, val] : counter_values) {
                    std::string series_name = event_name;
                    if (counter_values.size() > 1) {
                        series_name += "." + arg_key;
                    }
                    auto& cs = model.find_or_create_counter_series(ev.pid, series_name);
                    cs.points.push_back({ev.ts, val});
                }
            } catch (...) {}
        }

        // Handle flow events
        if (ev.ph == Phase::FlowStart || ev.ph == Phase::FlowStep || ev.ph == Phase::FlowEnd) {
            model.add_flow_event(ev.id, ev_idx);
        }

        model.add_event(ev);

        if (ev.ph != Phase::Counter) {
            auto& proc = model.get_or_create_process(ev.pid);
            auto& thread = proc.get_or_create_thread(ev.tid);
            thread.event_indices.push_back(ev_idx);
        } else {
            model.get_or_create_process(ev.pid);
        }
    }
}

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

    // Allocate with simdjson padding
    std::string content(file_size + simdjson::SIMDJSON_PADDING, '\0');
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
    model.intern_string("");

    if (on_progress_) on_progress_("Parsing JSON", 0.0f);

    double time_divisor = time_unit_ns_ ? 1000.0 : 1.0;
    uint64_t estimated_events = file_size / 100;

    simdjson::ondemand::parser parser;
    simdjson::padded_string_view psv(content.data(), file_size, content.size());

    try {
        auto doc = parser.iterate(psv);
        auto type = doc.type().value();

        if (type == simdjson::ondemand::json_type::array) {
            parse_events(doc.get_array().value(), model, time_divisor, on_progress_, estimated_events);
        } else if (type == simdjson::ondemand::json_type::object) {
            auto obj = doc.get_object().value();
            for (auto field : obj) {
                auto key = field.unescaped_key().value();
                if (key == "traceEvents") {
                    parse_events(field.value().get_array().value(), model, time_divisor, on_progress_,
                                 estimated_events);
                    break;
                }
            }
        }
    } catch (simdjson::simdjson_error& e) {
        // If we parsed some events, continue with what we have (truncated trace)
        if (model.events().empty()) {
            error_message_ = std::string("JSON parse error: ") + e.what();
            return false;
        }
    }

    // Free the raw JSON string before building the index
    content.clear();
    content.shrink_to_fit();

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

    double time_divisor = time_unit_ns_ ? 1000.0 : 1.0;
    uint64_t estimated_events = size / 100;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(data, size);

    try {
        auto doc = parser.iterate(padded);
        auto type = doc.type().value();

        if (type == simdjson::ondemand::json_type::array) {
            parse_events(doc.get_array().value(), model, time_divisor, on_progress_, estimated_events);
        } else if (type == simdjson::ondemand::json_type::object) {
            auto obj = doc.get_object().value();
            for (auto field : obj) {
                auto key = field.unescaped_key().value();
                if (key == "traceEvents") {
                    parse_events(field.value().get_array().value(), model, time_divisor, on_progress_,
                                 estimated_events);
                    break;
                }
            }
        }
    } catch (simdjson::simdjson_error& e) {
        if (model.events().empty()) {
            error_message_ = std::string("JSON parse error: ") + e.what();
            return false;
        }
    }

    if (on_progress_) on_progress_("Building index", 0.0f);
    model.build_index(on_progress_ ? [this](float p) { on_progress_("Building index", p); }
                                   : std::function<void(float)>{});

    if (on_progress_) on_progress_("Done", 1.0f);

    return true;
}
