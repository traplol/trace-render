#include "profile_io.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace {
using json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t u32(const json& value) {
    require(value.is_number_unsigned() && value.get<uint64_t>() <= UINT32_MAX, "expected an unsigned 32-bit integer");
    return value.get<uint32_t>();
}

int32_t i32(const json& value) {
    require(value.is_number_integer(), "expected a signed 32-bit integer");
    if (value.is_number_unsigned()) {
        require(value.get<uint64_t>() <= INT32_MAX, "signed 32-bit integer is out of range");
    } else {
        require(value.get<int64_t>() >= INT32_MIN && value.get<int64_t>() <= INT32_MAX,
                "signed 32-bit integer is out of range");
    }
    return value.get<int32_t>();
}

uint64_t u64(const json& value) {
    require(value.is_string(), "expected a decimal string for a 64-bit integer");
    const auto& text = value.get_ref<const std::string&>();
    uint64_t result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "invalid unsigned 64-bit decimal string: " + text);
    return result;
}

double number(const json& value) {
    require(value.is_number(), "expected a finite number");
    double result = value.get<double>();
    require(std::isfinite(result), "expected a finite number");
    return result;
}

std::optional<double> optional_time(const json& value) {
    if (value.is_null()) return std::nullopt;
    return number(value);
}

std::optional<uint64_t> optional_u64(const json& value) {
    if (value.is_null()) return std::nullopt;
    return u64(value);
}

json time_value(double value) {
    require(std::isfinite(value), "cannot save a non-finite timestamp or weight");
    return value;
}

json time_value(std::optional<double> value) {
    return value ? time_value(*value) : json(nullptr);
}

json integer_value(std::optional<uint64_t> value) {
    return value ? json(std::to_string(*value)) : json(nullptr);
}

void time_range(std::optional<double> start, std::optional<double> end) {
    require(!start || !end || *start <= *end, "end time precedes start time");
}

const json& array(const json& record, const char* key) {
    const auto& value = record.at(key);
    require(value.is_array(), std::string(key) + " must be an array");
    return value;
}

std::string identifier(const json& record, std::unordered_set<std::string>& ids) {
    auto id = record.at("id").get<std::string>();
    require(!id.empty() && ids.insert(id).second, "empty or duplicate identity: " + id);
    return id;
}

void reference(const std::string& id, const std::unordered_set<std::string>& ids, const char* kind) {
    require(ids.count(id) != 0, std::string("unknown ") + kind + " identity: " + id);
}

const char* kind_name(EventKind kind) {
    switch (kind) {
        case EventKind::Measured:
            return "measured";
        case EventKind::Sample:
            return "sample";
        case EventKind::SampledSpan:
            return "sampled_span";
    }
    throw std::runtime_error("invalid event kind");
}

const char* end_name(AllocationEnd end) {
    switch (end) {
        case AllocationEnd::Unknown:
            return "unknown";
        case AllocationEnd::Freed:
            return "freed";
        case AllocationEnd::LiveAtCaptureEnd:
            return "live_at_capture_end";
    }
    throw std::runtime_error("invalid allocation end state");
}
}  // namespace

bool serialize_profile(const TraceModel& model, std::string& data, std::string& error) {
    error.clear();
    data.clear();
    try {
        json root = {{"version", PROFILE_VERSION},
                     {"time_unit", "us"},
                     {"byte_unit", "bytes"},
                     {"strings", model.strings()},
                     {"args", model.args()}};
        for (const char* table : {"events", "stack_frames", "processes", "counters"}) root[table] = json::array();
        for (const auto& e : model.events()) {
            root["events"].push_back(
                {{"name", e.name_idx},
                 {"category", e.cat_idx},
                 {"phase", std::string(1, (char)e.ph)},
                 {"kind", kind_name(e.kind)},
                 {"ts", time_value(e.ts)},
                 {"duration", time_value(e.dur)},
                 {"pid", e.pid},
                 {"tid", e.tid},
                 {"id", std::to_string(e.id)},
                 {"process_instance", e.process_instance_id},
                 {"args", e.args_idx},
                 {"stack_frame", e.stack_frame_id},
                 {"weight", e.sample_weight < 0 ? json(nullptr) : time_value(e.sample_weight)},
                 {"weight_unit", e.sample_weight_unit},
                 {"estimated_cpu_us", e.sample_cpu_time < 0 ? json(nullptr) : time_value(e.sample_cpu_time)}});
        }
        for (const auto& f : model.stack_frames()) {
            root["stack_frames"].push_back({{"id", f.id_idx},
                                            {"name", f.name_idx},
                                            {"category", f.cat_idx},
                                            {"parent", f.parent_id},
                                            {"valid", f.valid},
                                            {"module", f.module_id},
                                            {"symbol", f.symbol_id},
                                            {"address", integer_value(f.address)},
                                            {"source_file", f.source_file},
                                            {"source_line", f.source_line},
                                            {"symbol_resolved", f.symbol_resolved}});
        }
        for (const auto& p : model.processes()) {
            json threads = json::array();
            for (const auto& t : p.threads)
                threads.push_back({{"tid", t.tid}, {"name", t.name}, {"sort_index", t.sort_index}});
            root["processes"].push_back(
                {{"pid", p.pid}, {"name", p.name}, {"sort_index", p.sort_index}, {"threads", threads}});
        }
        for (const auto& c : model.counter_series()) {
            json points = json::array();
            for (const auto& p : c.points) points.push_back({time_value(p.first), time_value(p.second)});
            root["counters"].push_back({{"pid", c.pid}, {"name", c.name}, {"unit", c.unit}, {"points", points}});
        }
        const auto& p = model.profile();
        const auto& c = p.capabilities;
        const auto& q = p.quality;
        root["profile"] = {{"source_format", p.source_format},
                           {"source_name", p.source_name},
                           {"converter", p.converter},
                           {"capture_start_ts", time_value(p.capture_start_ts)},
                           {"capture_end_ts", time_value(p.capture_end_ts)},
                           {"capabilities",
                            {{"native_cpu_samples", c.native_cpu_samples},
                             {"managed_cpu_samples", c.managed_cpu_samples},
                             {"native_allocation_history", c.native_allocation_history},
                             {"managed_allocation_history", c.managed_allocation_history},
                             {"managed_heap_snapshots", c.managed_heap_snapshots},
                             {"managed_survival", c.managed_survival}}},
                           {"quality",
                            {{"incomplete_capture", q.incomplete_capture},
                             {"sampled_cpu", q.sampled_cpu},
                             {"sampled_allocations", q.sampled_allocations},
                             {"unresolved_symbols", q.unresolved_symbols},
                             {"lost_events", integer_value(q.lost_events)},
                             {"warnings", q.warnings}}}};
        auto& profile = root["profile"];
        for (const char* table :
             {"process_instances", "heap_instances", "modules", "allocations", "managed_snapshots", "managed_survival"})
            profile[table] = json::array();
        for (const auto& process : p.processes)
            profile["process_instances"].push_back({{"id", process.id},
                                                    {"pid", process.pid},
                                                    {"start_ts", time_value(process.start_ts)},
                                                    {"end_ts", time_value(process.end_ts)}});
        for (const auto& heap : p.heaps)
            profile["heap_instances"].push_back({{"id", heap.id},
                                                 {"process_id", heap.process_id},
                                                 {"start_ts", time_value(heap.start_ts)},
                                                 {"end_ts", time_value(heap.end_ts)}});
        for (const auto& m : p.modules)
            profile["modules"].push_back({{"id", m.id},
                                          {"process_id", m.process_id},
                                          {"name", m.name},
                                          {"path", m.path},
                                          {"build_id", m.build_id},
                                          {"load_address", std::to_string(m.load_address)},
                                          {"size_bytes", std::to_string(m.size_bytes)},
                                          {"load_ts", time_value(m.load_ts)},
                                          {"unload_ts", time_value(m.unload_ts)}});
        for (const auto& a : p.allocations)
            profile["allocations"].push_back({{"id", a.id},
                                              {"process_id", a.process_id},
                                              {"heap_id", a.heap_id},
                                              {"address", integer_value(a.address)},
                                              {"size_bytes", std::to_string(a.size_bytes)},
                                              {"stack_frame_id", a.stack_frame_id},
                                              {"type_id", a.type_id},
                                              {"kind", a.kind == AllocationKind::Native ? "native" : "managed"},
                                              {"allocated_ts", time_value(a.allocated_ts)},
                                              {"freed_ts", time_value(a.freed_ts)},
                                              {"end_state", end_name(a.end_state)}});
        for (const auto& snapshot : p.managed_snapshots) {
            json types = json::array();
            for (const auto& t : snapshot.types)
                types.push_back({{"type_id", t.type_id},
                                 {"name", t.name},
                                 {"object_count", std::to_string(t.object_count)},
                                 {"size_bytes", std::to_string(t.size_bytes)}});
            profile["managed_snapshots"].push_back({{"id", snapshot.id},
                                                    {"process_id", snapshot.process_id},
                                                    {"ts", time_value(snapshot.ts)},
                                                    {"live_bytes", integer_value(snapshot.live_bytes)},
                                                    {"object_count", integer_value(snapshot.object_count)},
                                                    {"types", types}});
        }
        for (const auto& o : p.managed_survival)
            profile["managed_survival"].push_back({{"allocation_id", o.allocation_id},
                                                   {"ts", time_value(o.ts)},
                                                   {"survived", o.survived},
                                                   {"address", integer_value(o.address)}});
        data = std::string(PROFILE_MAGIC) + root.dump();
        return true;
    } catch (const std::exception& e) {
        error = std::string("Could not save TraceRender profile: ") + e.what();
        return false;
    }
}

bool read_profile(std::string_view data, TraceModel& model, std::string& error) {
    error.clear();
    std::string location = "header";
    try {
        require(data.substr(0, PROFILE_MAGIC.size()) == PROFILE_MAGIC, "expected TRPROFILE header");
        const auto root = json::parse(data.substr(PROFILE_MAGIC.size()));
        auto version = u32(root.at("version"));
        require(version == PROFILE_VERSION,
                "unsupported version " + std::to_string(version) + "; this build supports version 1");
        require(root.at("time_unit") == "us" && root.at("byte_unit") == "bytes",
                "version 1 requires time_unit us and byte_unit bytes");
        TraceModel parsed;
        location = "strings";
        const auto& strings = array(root, "strings");
        require(!strings.empty() && strings[0] == "", "strings[0] must be the empty string");
        require(strings.size() < UINT32_MAX, "too many strings");
        for (size_t i = 0; i < strings.size(); ++i)
            require(parsed.intern_string(strings[i].get<std::string>()) == i, "string table contains duplicates");
        auto string_index = [&](const json& value, bool nullable = false) {
            uint32_t index = u32(value);
            require(index < strings.size() || (nullable && index == UINT32_MAX), "string index is out of range");
            return index;
        };
        location = "args";
        for (const auto& args : array(root, "args")) parsed.add_args(args.get<std::string>());
        ProfileData profile;
        location = "profile";
        const auto& p = root.at("profile");
        profile.source_format = p.at("source_format").get<std::string>();
        profile.source_name = p.at("source_name").get<std::string>();
        profile.converter = p.at("converter").get<std::string>();
        profile.capture_start_ts = optional_time(p.at("capture_start_ts"));
        profile.capture_end_ts = optional_time(p.at("capture_end_ts"));
        time_range(profile.capture_start_ts, profile.capture_end_ts);
        const auto& c = p.at("capabilities");
        profile.capabilities = {
            c.at("native_cpu_samples").get<bool>(),        c.at("managed_cpu_samples").get<bool>(),
            c.at("native_allocation_history").get<bool>(), c.at("managed_allocation_history").get<bool>(),
            c.at("managed_heap_snapshots").get<bool>(),    c.at("managed_survival").get<bool>()};
        const auto& q = p.at("quality");
        profile.quality = {q.at("incomplete_capture").get<bool>(),  q.at("sampled_cpu").get<bool>(),
                           q.at("sampled_allocations").get<bool>(), q.at("unresolved_symbols").get<bool>(),
                           optional_u64(q.at("lost_events")),       q.at("warnings").get<std::vector<std::string>>()};
        std::unordered_set<std::string> processes, heaps, modules, frames, allocations, snapshots;
        std::unordered_map<std::string, size_t> managed_allocations;
        std::unordered_map<std::string, std::string> heap_processes;
        for (const auto& item : array(p, "process_instances")) {
            location = "profile.process_instances[" + std::to_string(profile.processes.size()) + "]";
            ProfileProcess process{identifier(item, processes), u32(item.at("pid")), optional_time(item.at("start_ts")),
                                   optional_time(item.at("end_ts"))};
            time_range(process.start_ts, process.end_ts);
            profile.processes.push_back(std::move(process));
        }
        for (const auto& item : array(p, "heap_instances")) {
            location = "profile.heap_instances[" + std::to_string(profile.heaps.size()) + "]";
            ProfileHeap heap{identifier(item, heaps), item.at("process_id").get<std::string>(),
                             optional_time(item.at("start_ts")), optional_time(item.at("end_ts"))};
            reference(heap.process_id, processes, "process");
            time_range(heap.start_ts, heap.end_ts);
            heap_processes[heap.id] = heap.process_id;
            profile.heaps.push_back(std::move(heap));
        }
        for (const auto& item : array(p, "modules")) {
            location = "profile.modules[" + std::to_string(profile.modules.size()) + "]";
            ProfileModule module{identifier(item, modules),
                                 item.at("process_id").get<std::string>(),
                                 item.at("name").get<std::string>(),
                                 item.at("path").get<std::string>(),
                                 item.at("build_id").get<std::string>(),
                                 u64(item.at("load_address")),
                                 u64(item.at("size_bytes")),
                                 optional_time(item.at("load_ts")),
                                 optional_time(item.at("unload_ts"))};
            reference(module.process_id, processes, "process");
            time_range(module.load_ts, module.unload_ts);
            require(module.size_bytes <= UINT64_MAX - module.load_address, "module address range overflows");
            profile.modules.push_back(std::move(module));
        }
        std::vector<bool> declared_valid;
        for (const auto& item : array(root, "stack_frames")) {
            location = "stack_frames[" + std::to_string(parsed.stack_frames().size()) + "]";
            StackFrame f;
            f.id_idx = string_index(item.at("id"));
            const auto& id = parsed.get_string(f.id_idx);
            require(!id.empty() && frames.insert(id).second, "empty or duplicate stack frame identity: " + id);
            f.name_idx = string_index(item.at("name"));
            f.cat_idx = string_index(item.at("category"));
            f.parent_id = string_index(item.at("parent"), true);
            f.valid = item.at("valid").get<bool>();
            declared_valid.push_back(f.valid);
            f.module_id = string_index(item.at("module"), true);
            if (f.module_id != UINT32_MAX) reference(parsed.get_string(f.module_id), modules, "module");
            f.symbol_id = string_index(item.at("symbol"), true);
            f.address = optional_u64(item.at("address"));
            f.source_file = string_index(item.at("source_file"));
            f.source_line = u32(item.at("source_line"));
            require(f.source_line <= INT_MAX, "source line exceeds supported range");
            f.symbol_resolved = item.at("symbol_resolved").get<bool>();
            parsed.add_stack_frame(f);
        }
        for (const auto& item : array(root, "events")) {
            location = "events[" + std::to_string(parsed.events().size()) + "]";
            TraceEvent e;
            e.name_idx = string_index(item.at("name"));
            e.cat_idx = string_index(item.at("category"));
            const auto phase = item.at("phase").get<std::string>();
            require(phase.size() == 1, "phase must be one character");
            e.ph = phase_from_char(phase[0]);
            require(e.ph != Phase::Unknown || phase == "?", "unknown event phase: " + phase);
            const auto kind = item.at("kind").get<std::string>();
            require(kind == "measured" || kind == "sample" || kind == "sampled_span", "unknown event kind: " + kind);
            e.kind = kind == "sample"         ? EventKind::Sample
                     : kind == "sampled_span" ? EventKind::SampledSpan
                                              : EventKind::Measured;
            require((e.ph == Phase::Sample) == (e.kind == EventKind::Sample), "sample kind requires phase P");
            require(e.kind != EventKind::SampledSpan || e.ph == Phase::DurationBegin || e.ph == Phase::DurationEnd ||
                        e.ph == Phase::Complete,
                    "sampled_span kind requires a B, E, or X interval");
            e.ts = number(item.at("ts"));
            e.dur = number(item.at("duration"));
            require(e.dur >= 0 && std::isfinite(e.ts + e.dur), "invalid event duration");
            require(e.kind != EventKind::Sample || e.dur == 0, "point samples cannot have duration");
            e.pid = u32(item.at("pid"));
            e.tid = u32(item.at("tid"));
            e.id = u64(item.at("id"));
            e.process_instance_id = string_index(item.at("process_instance"), true);
            if (e.process_instance_id != UINT32_MAX)
                reference(parsed.get_string(e.process_instance_id), processes, "process");
            e.args_idx = u32(item.at("args"));
            require(e.args_idx == UINT32_MAX || e.args_idx < parsed.args().size(), "args index is out of range");
            e.stack_frame_id = string_index(item.at("stack_frame"), true);
            e.sample_weight = optional_time(item.at("weight")).value_or(-1);
            e.sample_weight_unit = string_index(item.at("weight_unit"));
            e.sample_cpu_time = optional_time(item.at("estimated_cpu_us")).value_or(-1);
            require(item.at("weight").is_null() || e.sample_weight >= 0, "weight must be nonnegative or null");
            require(item.at("estimated_cpu_us").is_null() || e.sample_cpu_time >= 0,
                    "estimated CPU must be nonnegative or null");
            uint32_t index = parsed.add_event(e);
            if (e.ph == Phase::FlowStart || e.ph == Phase::FlowStep || e.ph == Phase::FlowEnd)
                parsed.add_flow_event(e.id, index);
        }
        std::unordered_set<uint32_t> display_processes;
        for (const auto& item : array(root, "processes")) {
            location = "processes";
            auto pid = u32(item.at("pid"));
            require(display_processes.insert(pid).second, "duplicate display process");
            auto& process = parsed.get_or_create_process(pid);
            process.name = item.at("name").get<std::string>();
            process.sort_index = i32(item.at("sort_index"));
            std::unordered_set<uint32_t> tids;
            for (const auto& thread : array(item, "threads")) {
                auto tid = u32(thread.at("tid"));
                require(tids.insert(tid).second, "duplicate display thread");
                auto& t = process.get_or_create_thread(tid);
                t.name = thread.at("name").get<std::string>();
                t.sort_index = i32(thread.at("sort_index"));
            }
        }
        std::unordered_map<uint32_t, std::unordered_set<std::string>> counter_names;
        for (const auto& item : array(root, "counters")) {
            location = "counters";
            auto pid = u32(item.at("pid"));
            auto name = item.at("name").get<std::string>();
            require(counter_names[pid].insert(name).second, "duplicate counter series");
            auto& counter = parsed.find_or_create_counter_series(pid, name);
            counter.unit = item.at("unit").get<std::string>();
            for (const auto& point : array(item, "points")) {
                require(point.is_array() && point.size() == 2, "counter point must contain timestamp and value");
                counter.points.emplace_back(number(point[0]), number(point[1]));
            }
        }
        for (const auto& item : array(p, "allocations")) {
            location = "profile.allocations[" + std::to_string(profile.allocations.size()) + "]";
            AllocationLifetime a;
            a.id = identifier(item, allocations);
            a.process_id = item.at("process_id").get<std::string>();
            reference(a.process_id, processes, "process");
            a.heap_id = item.at("heap_id").get<std::string>();
            if (!a.heap_id.empty()) {
                reference(a.heap_id, heaps, "heap");
                require(heap_processes.at(a.heap_id) == a.process_id,
                        "allocation and heap belong to different process lifetimes");
            }
            a.address = optional_u64(item.at("address"));
            a.size_bytes = u64(item.at("size_bytes"));
            a.stack_frame_id = item.at("stack_frame_id").get<std::string>();
            if (!a.stack_frame_id.empty()) reference(a.stack_frame_id, frames, "allocation stack frame");
            a.type_id = item.at("type_id").get<std::string>();
            const auto kind = item.at("kind").get<std::string>();
            require(kind == "native" || kind == "managed", "unknown allocation kind: " + kind);
            a.kind = kind == "native" ? AllocationKind::Native : AllocationKind::Managed;
            if (a.kind == AllocationKind::Managed) managed_allocations.emplace(a.id, profile.allocations.size());
            a.allocated_ts = optional_time(item.at("allocated_ts"));
            a.freed_ts = optional_time(item.at("freed_ts"));
            time_range(a.allocated_ts, a.freed_ts);
            const auto state = item.at("end_state").get<std::string>();
            require(state == "unknown" || state == "freed" || state == "live_at_capture_end",
                    "unknown allocation end state: " + state);
            a.end_state = state == "freed"                 ? AllocationEnd::Freed
                          : state == "live_at_capture_end" ? AllocationEnd::LiveAtCaptureEnd
                                                           : AllocationEnd::Unknown;
            require(!a.freed_ts || a.end_state == AllocationEnd::Freed, "free timestamp requires freed end state");
            if (a.end_state == AllocationEnd::LiveAtCaptureEnd) time_range(a.allocated_ts, profile.capture_end_ts);
            profile.allocations.push_back(std::move(a));
        }
        for (const auto& item : array(p, "managed_snapshots")) {
            location = "profile.managed_snapshots[" + std::to_string(profile.managed_snapshots.size()) + "]";
            ManagedSnapshot snapshot;
            snapshot.id = identifier(item, snapshots);
            snapshot.process_id = item.at("process_id").get<std::string>();
            reference(snapshot.process_id, processes, "process");
            snapshot.ts = number(item.at("ts"));
            snapshot.live_bytes = optional_u64(item.at("live_bytes"));
            snapshot.object_count = optional_u64(item.at("object_count"));
            std::unordered_set<std::string> type_ids;
            for (const auto& type : array(item, "types")) {
                auto id = type.at("type_id").get<std::string>();
                require(!id.empty() && type_ids.insert(id).second, "empty or duplicate snapshot type identity");
                snapshot.types.push_back(
                    {id, type.at("name").get<std::string>(), u64(type.at("object_count")), u64(type.at("size_bytes"))});
            }
            profile.managed_snapshots.push_back(std::move(snapshot));
        }
        for (const auto& item : array(p, "managed_survival")) {
            location = "profile.managed_survival[" + std::to_string(profile.managed_survival.size()) + "]";
            ManagedSurvivalObservation observation{item.at("allocation_id").get<std::string>(), number(item.at("ts")),
                                                   item.at("survived").get<bool>(), optional_u64(item.at("address"))};
            auto allocation = managed_allocations.find(observation.allocation_id);
            require(allocation != managed_allocations.end(),
                    "unknown managed allocation identity: " + observation.allocation_id);
            const auto& lifetime = profile.allocations[allocation->second];
            require(!lifetime.allocated_ts || observation.ts >= *lifetime.allocated_ts,
                    "GC observation precedes known allocation time");
            if (observation.survived)
                require(!lifetime.freed_ts || observation.ts <= *lifetime.freed_ts,
                        "survival observation follows known free time");
            else {
                require(!lifetime.freed_ts || observation.ts >= *lifetime.freed_ts,
                        "death observation precedes known free time");
                require(lifetime.end_state != AllocationEnd::LiveAtCaptureEnd ||
                            (profile.capture_end_ts && observation.ts > *profile.capture_end_ts),
                        "death observation contradicts live_at_capture_end");
            }
            profile.managed_survival.push_back(std::move(observation));
        }
        parsed.set_profile(std::move(profile));
        location = "stack_frames";
        parsed.build_index();
        for (size_t i = 0; i < declared_valid.size(); ++i)
            require(!declared_valid[i] || parsed.stack_frames()[i].valid,
                    "frame " + parsed.get_string(parsed.stack_frames()[i].id_idx) + " has a missing or cyclic parent");
        model = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        error = "Invalid TraceRender profile at " + location + ": " + e.what();
        return false;
    }
}

bool write_profile(const std::string& filepath, const TraceModel& model, std::string& error) {
    std::string data;
    if (!serialize_profile(model, data, error)) return false;
    std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(data.data(), (std::streamsize)data.size()) || !file.flush()) {
        error = "Could not write TraceRender profile: " + filepath;
        return false;
    }
    return true;
}
