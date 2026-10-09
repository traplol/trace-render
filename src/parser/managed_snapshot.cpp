#include "managed_snapshot.h"
#include "diagsession_container.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct SerializedType {
    uint32_t version, minimum;
    std::string name;
};

// This is a reader for the observed GCDump layout, not a general FastSerialization deserializer.
class GcdumpReader {
public:
    explicit GcdumpReader(std::string_view data) : data_(data), bytes_(data) {
        require(data.size() >= 12, "Truncated GCDump trailer");
        const size_t trailer = bytes_.u32(data.size() - 4);
        require(trailer == data.size() - 8, "Unsupported GCDump trailer");
        references_ = bytes_.u32(trailer);
        require(references_ < trailer && trailer - references_ >= 4, "Invalid GCDump references");
        const size_t count = bytes_.u32(references_);
        require(count == (trailer - references_ - 4) / 4 && (trailer - references_ - 4) % 4 == 0,
                "Invalid GCDump reference table");
    }

    uint8_t byte() { return (uint8_t)take(1)[0]; }
    uint32_t u32() {
        auto at = position_;
        take(4);
        return bytes_.u32(at);
    }
    uint64_t u64() {
        auto at = position_;
        take(8);
        return bytes_.u64(at);
    }
    float weight() {
        auto bits = u32();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        require(std::isfinite(value) && value > 0, "Invalid GCDump sampling multiplier");
        return value;
    }
    std::string_view take(size_t size) {
        require(position_ <= references_ && size <= references_ - position_, "Truncated GCDump data");
        auto result = data_.substr(position_, size);
        position_ += size;
        return result;
    }
    std::string text() {
        auto size = u32();
        if (size == UINT32_MAX) return {};
        require(size <= INT32_MAX, "Invalid GCDump string length");
        return std::string(take(size));
    }
    uint32_t count(size_t element_bytes) {
        auto value = u32();
        require(value <= INT32_MAX && value <= (references_ - position_) / element_bytes, "Invalid GCDump count");
        return value;
    }
    void tag(uint8_t expected) { require(byte() == expected, "Unexpected GCDump serialization tag"); }
    uint8_t peek() const { return bytes_.u8(position_); }

    void begin(const char* name, uint32_t version, uint32_t minimum) {
        tag(4);
        const size_t type_offset = position_;
        SerializedType type;
        auto type_tag = byte();
        if (type_tag == 2) {
            auto found = types_.find(u32());
            require(found != types_.end(), "Unknown GCDump type reference");
            type = found->second;
        } else {
            require(type_tag == 4, "Unsupported GCDump type definition");
            tag(1);
            type.version = u32();
            type.minimum = u32();
            type.name = text();
            tag(6);
            types_.emplace(type_offset, type);
        }
        require(type.name == name && type.version == version && type.minimum == minimum,
                "Unsupported GCDump object type or version");
    }

    void metadata() {
        if (peek() == 1)
            byte();
        else {
            begin("JSHeapInfo", 0, 0);
            tag(6);
        }
        if (peek() == 1)
            byte();
        else {
            begin("DotNetHeapInfo", 0, 0);
            u64();  // segment bytes, not live object bytes
            auto segments = count(50);
            for (uint32_t i = 0; i < segments; ++i) {
                begin("GCHeapDumpSegment", 0, 0);
                take(48);  // observed v0: start, end, and four generation boundaries
                tag(6);
            }
            tag(6);
        }
    }

    void end() {
        // Post-v8 fields are tagged. Interop data is a length-delimited region and
        // has no effect on the graph's recorded count/size summaries.
        while (peek() != 6) {
            switch (byte()) {
                case 1:
                    break;
                case 8:
                    take(1);
                    break;
                case 9:
                    take(2);
                    break;
                case 10:
                    take(4);
                    break;
                case 11:
                    take(8);
                    break;
                case 13:
                    text();
                    break;
                case 12: {
                    const auto index = u32();
                    require(index < bytes_.u32(references_), "Invalid GCDump region reference");
                    const auto target = bytes_.u32(references_ + 4 + size_t(index) * 4);
                    require(target >= position_ && target < references_, "Invalid GCDump region boundary");
                    position_ = target;
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported GCDump tagged field");
            }
        }
        tag(6);
    }
    void finish() {
        end();
        tag(6);  // stream object terminator
        require(position_ == references_, "Unexpected GCDump data after heap graph");
    }

private:
    std::string_view data_;
    EtlBytes bytes_;
    size_t position_ = 0, references_ = 0;
    std::map<size_t, SerializedType> types_;
};

int32_t compressed(std::string_view data, size_t& position) {
    require(position < data.size(), "Truncated GCDump node");
    uint8_t b = (uint8_t)data[position++];
    int64_t value = b & 0x7f;
    if (b & 0x40) value -= 128;
    for (int length = 1; b & 0x80; ++length) {
        require(length < 5 && position < data.size(), "Invalid GCDump compressed integer");
        b = (uint8_t)data[position++];
        value = value * 128 + (b & 0x7f);
    }
    require(value >= INT32_MIN && value <= INT32_MAX, "GCDump compressed integer overflow");
    return (int32_t)value;
}
}  // namespace

bool read_gcdump(std::string_view bytes, ManagedSnapshot& snapshot, std::string& error,
                 const ImportProgress& progress) {
    snapshot = {};
    error.clear();
    try {
        GcdumpReader reader(bytes);
        require(reader.text() == "!FastSerialization.1", "Unsupported GCDump signature");
        reader.begin("GCHeapDump", 10, 8);
        reader.begin("Graphs.MemoryGraph", 1, 0);
        const auto recorded_total = reader.u64();
        const auto root = reader.u32();
        const auto type_count = reader.count(12);
        ManagedSnapshot result;
        std::vector<int32_t> nominal_sizes;
        for (uint32_t i = 0; i < type_count; ++i) {
            auto name = reader.text();
            nominal_sizes.push_back((int32_t)reader.u32());
            reader.text();  // module name; type index remains the capture-local identity
            result.types.push_back({std::to_string(i), std::move(name)});
        }
        const auto node_count = reader.count(12);
        require(node_count == 0 || root < node_count, "Invalid GCDump graph root");
        EtlBytes offsets(reader.take(size_t(node_count) * 4));
        const auto blob_size = reader.count(1);
        const auto blob = reader.take(blob_size);
        const auto address_count = reader.count(8);
        require(address_count == node_count, "GCDump node/address counts differ");
        EtlBytes addresses(reader.take(size_t(address_count) * 8));
        reader.end();
        reader.byte();  // legacy Is64Bit
        result.average_count_multiplier = reader.weight();
        result.average_size_multiplier = reader.weight();
        result.sampled = result.average_count_multiplier != 1 || result.average_size_multiplier != 1;
        reader.metadata();
        reader.text();  // collection log is not structured completeness evidence
        reader.u64();   // wall clock; the diagsession manifest supplies capture-relative time
        reader.text();
        reader.text();
        reader.u32();
        reader.u64();
        reader.u64();
        const auto multiplier_count = reader.count(4);
        require(multiplier_count == 0 || multiplier_count == type_count, "GCDump type multiplier count differs");
        if (multiplier_count) result.sampled = true;
        for (uint32_t i = 0; i < multiplier_count; ++i) result.types[i].count_multiplier = reader.weight();
        reader.finish();

        uint64_t total = 0, objects = 0;
        size_t work = size_t(blob_size) * 2 + size_t(node_count) * 3;
        for (uint32_t i = 0; i < node_count; ++i) {
            if (i % 4096 == 0 && progress && !progress("Reading managed heap objects", float(i) / node_count))
                throw std::runtime_error("Import canceled");
            size_t at = offsets.u32(size_t(i) * 4), begin = at;
            const auto type_size = compressed(blob, at);
            require(type_size >= 0 && (uint32_t(type_size) >> 1) < type_count, "Invalid GCDump node type");
            const auto type = uint32_t(type_size) >> 1;
            const auto size = type_size & 1 ? compressed(blob, at) : nominal_sizes[type];
            require(size >= 0, "Invalid GCDump node size");
            const auto children = compressed(blob, at);
            require(children >= 0 && size_t(children) <= blob.size() - at, "Invalid GCDump child count");
            for (int32_t c = 0; c < children; ++c) {
                const int64_t child = int64_t(i) + compressed(blob, at);
                require(child >= 0 && child < node_count, "Invalid GCDump child reference");
            }
            require(at - begin <= work, "Excessive duplicate GCDump node data");
            work -= at - begin;
            const auto address = addresses.u64(size_t(i) * 8);
            // VS and PerfView use addressless, zero-size nodes to group roots and references.
            const auto& name = result.types[type].name;
            if (!address && size == 0 && name.size() >= 2 && name.front() == '[' && name.back() == ']') continue;
            ++objects;
            total += uint32_t(size);  // node count and int32 sizes bound this below uint64 overflow
            ++result.types[type].object_count;
            result.types[type].size_bytes += uint32_t(size);
            if (!address || type == 0) result.incomplete = true;
        }
        require(total == recorded_total, "GCDump graph size does not match its recorded total");
        result.live_bytes = total;
        result.object_count = objects;
        if (result.incomplete)
            result.warnings.push_back(
                "Snapshot contains objects with an unknown address or type. Complete heap coverage is unproven.");
        snapshot = std::move(result);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Could not read managed snapshot: ") + e.what();
        return false;
    }
}

bool read_managed_snapshots(const DiagsessionContainer& container, ProfileData& profile, std::string& error,
                            const ImportProgress& progress) {
    using json = nlohmann::json;
    bool canceled = false;
    auto check_progress = [&](const char* phase, float value) {
        if (progress && !progress(phase, value)) canceled = true;
        return !canceled;
    };
    auto partial = [&](const std::string& message) {
        profile.quality.incomplete_capture = true;
        profile.quality.warnings.push_back(message);
    };
    const auto& resources = container.resources();
    const auto recorded_process_count = profile.processes.size();
    for (size_t m = 0; m < resources.size(); ++m) {
        if (resources[m].type != "MemoryProfiler.Manifest" || resources[m].directory) continue;
        try {
            std::vector<uint8_t> data;
            if (!container.read_resource(m, data, error,
                                         [&] { return !check_progress("Reading managed manifest", 0); }))
                throw std::runtime_error(error);
            const auto manifest = json::parse(data);
            require(manifest.at("Version") == 1, "Unsupported managed memory manifest version");
            if (!manifest.value("IsManagedEnabled", false)) continue;
            for (const auto& item : manifest.at("Snapshots")) {
                const auto raw_time = item.at("SnapshotTime");
                const auto raw_pid = item.at("ProcessId");
                require(raw_time.is_number_unsigned() && raw_pid.is_number_unsigned(), "Invalid snapshot time or PID");
                require(raw_pid.get<uint64_t>() <= UINT32_MAX, "Invalid snapshot PID");
                // MemoryProfiler manifest v1 timestamps are capture-relative nanoseconds.
                const auto time = raw_time.get<uint64_t>() / 1000.0;
                const auto pid = raw_pid.get<uint32_t>();
                for (const auto& heap : item.at("Heaps")) {
                    if (heap.at("Type") != "PROFILER_MANAGED") continue;
                    const auto name = heap.at("GCDumpFileName").get<std::string>();
                    try {
                        size_t resource_index = resources.size();
                        for (size_t r = 0; r < resources.size(); ++r) {
                            if (resources[r].type != "MemoryProfiler.GCDump" || resources[r].name != name ||
                                resources[r].directory)
                                continue;
                            require(resource_index == resources.size(), "Ambiguous managed snapshot resource name");
                            resource_index = r;
                        }
                        require(resource_index < resources.size(), "Managed snapshot resource is missing");
                        if (!container.read_resource(resource_index, data, error,
                                                     [&] { return !check_progress("Extracting managed snapshot", 0); }))
                            throw std::runtime_error(error);
                        ManagedSnapshot snapshot;
                        if (!read_gcdump({reinterpret_cast<const char*>(data.data()), data.size()}, snapshot, error,
                                         check_progress))
                            throw std::runtime_error(error);
                        snapshot.id =
                            "managed/" + std::to_string(m) + "/" + std::to_string(profile.managed_snapshots.size());
                        snapshot.ts = time;
                        size_t matches = 0;
                        for (size_t p = 0; p < recorded_process_count; ++p) {
                            const auto& process = profile.processes[p];
                            if (process.pid != pid || (process.start_ts && time < *process.start_ts) ||
                                (process.end_ts && time >= *process.end_ts))
                                continue;
                            snapshot.process_id = process.id;
                            ++matches;
                        }
                        if (matches != 1) {
                            snapshot.process_id = snapshot.id + "/process";
                            profile.processes.push_back({snapshot.process_id, pid, {}, {}});
                            snapshot.incomplete = true;
                            snapshot.warnings.push_back(
                                "The snapshot PID could not be matched to one recorded process lifetime.");
                        }
                        profile.managed_snapshots.push_back(std::move(snapshot));
                    } catch (const std::exception& e) {
                        if (canceled) {
                            error = "Import canceled";
                            return false;
                        }
                        partial("Managed snapshot " + name + " was skipped: " + e.what());
                    }
                }
            }
        } catch (const std::exception& e) {
            if (canceled) {
                error = "Import canceled";
                return false;
            }
            partial("Managed memory manifest was skipped: " + std::string(e.what()));
        }
    }
    std::stable_sort(profile.managed_snapshots.begin(), profile.managed_snapshots.end(),
                     [](const auto& a, const auto& b) { return a.ts < b.ts; });
    profile.capabilities.managed_heap_snapshots = !profile.managed_snapshots.empty();
    error.clear();
    return true;
}
