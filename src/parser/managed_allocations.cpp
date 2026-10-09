#include "managed_allocations.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
constexpr const char* allocation_provider = "8bc9e67b-ca34-4b9a-9442-8f75403f357b";
constexpr const char* runtime_provider = "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";

void warn(ProfileData& profile, const std::string& text) {
    auto& warnings = profile.quality.warnings;
    if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text);
}
std::string bytes_id(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        result += digits[b >> 4];
        result += digits[b & 15];
    }
    return result;
}
size_t after_utf16(const EtlBytes& bytes, size_t at) {
    while (bytes.u16(at)) at += 2;
    return at + 2;
}
}  // namespace

bool decode_managed_allocation(const EtlRecord& r, ManagedAllocationEvent& event) {
    const bool allocation = r.provider == allocation_provider;
    if (!allocation && (r.provider != runtime_provider || (r.event_id != 1 && r.event_id != 2 && r.event_id != 187)))
        return false;
    event = {};
    event.raw_qpc = r.qpc;
    event.pid = r.pid.value_or(0);
    if (!r.pid) return true;
    try {
        EtlBytes p(r.payload);
        if (allocation) {
            if (r.pointer_size != 8) return true;
            if (r.event_id == 1 && r.version == 4) {
                const size_t type_size = p.u32(12);
                if (type_size < 12 || r.payload.size() != 20 + type_size) return true;
                event.object_id = p.u64(0);
                event.size_bytes = p.u32(8);
                if (!event.object_id) return true;
                // The type descriptor stays opaque; it is not an allocation origin.
                event.type_id = "vs-type/" + bytes_id(p.slice(16, type_size));
                const EtlExtendedStack* stack = nullptr;
                size_t stacks = 0;
                for (const auto& item : r.extensions) {
                    if (item.stack) {
                        stack = &*item.stack;
                        ++stacks;
                    }
                }
                // Only the observed single inline stack has unambiguous ancestry.
                if (stacks == 1 && stack->pointer_size == 8) event.addresses = stack->addresses;
                event.kind = ManagedAllocationEventKind::Allocate;
            } else if (r.event_id == 2 && r.version == 1) {
                const size_t count = p.u32(4);
                if (p.u32(0) != 0 || r.payload.size() != 8 + count * 8) return true;
                for (size_t i = 0; i < count; ++i) event.collected_ids.push_back(p.u64(8 + i * 8));
                event.kind = ManagedAllocationEventKind::Collected;
            }
        } else if (r.event_id == 187 && r.version == 0) {
            // CLR RuntimeInformationStart: runtime manifest and TraceEvent schema.
            if (after_utf16(p, after_utf16(p, 25) + 16) != r.payload.size()) return true;
            event.clr_instance = p.u16(0);
            event.runtime_sku = p.u16(2);
            event.runtime_major = p.u16(12);
            event.kind = ManagedAllocationEventKind::Runtime;
        } else if (r.event_id == 1 && r.version == 2 && r.payload.size() == 26) {
            event.gc_number = p.u32(0);
            event.gc_depth = p.u32(4);
            event.gc_type = p.u32(12);
            event.clr_instance = p.u16(16);
            event.kind = ManagedAllocationEventKind::GcStart;
        } else if (r.event_id == 2 && r.version == 1 && r.payload.size() == 10) {
            event.gc_number = p.u32(0);
            event.gc_depth = p.u32(4);
            event.clr_instance = p.u16(8);
            event.kind = ManagedAllocationEventKind::GcEnd;
        }
    } catch (const std::exception&) {
        event.kind = ManagedAllocationEventKind::Gap;
    }
    return true;
}

void build_managed_allocations(std::vector<ManagedAllocationEvent>& events, ProfileData& profile,
                               bool complete_event_stream, const ImportProgress& progress) {
    struct Process {
        std::set<std::tuple<uint16_t, uint16_t, uint16_t>> runtimes;
        std::set<uint16_t> gc_instances;
        std::set<uint64_t> allocation_ids;
        std::map<uint64_t, size_t> live;
        const ManagedAllocationEvent* gc = nullptr;
        std::optional<uint32_t> last_gc_number;
        std::vector<uint64_t> collected;
        std::vector<ManagedSurvivalObservation> observations;
        bool valid = true;
    };
    std::map<std::string, Process> processes;
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.raw_qpc < b.raw_qpc; });
    bool unknown_process_gap = false;
    for (size_t i = 0; i < events.size(); ++i) {
        if (i % 4096 == 0 && progress && !progress("Replaying managed allocations", float(i) / events.size()))
            throw std::runtime_error("Import canceled");
        const auto& event = events[i];
        auto& process = processes[event.process_id];
        switch (event.kind) {
            case ManagedAllocationEventKind::Runtime:
                process.runtimes.emplace(event.clr_instance, event.runtime_sku, event.runtime_major);
                break;
            case ManagedAllocationEventKind::Gap:
                process.valid = false;
                unknown_process_gap |= event.pid == 0;
                break;
            case ManagedAllocationEventKind::Allocate: {
                if (process.gc) process.valid = false;
                AllocationLifetime allocation;
                allocation.id = event.process_id + "/vs-allocation/" + std::to_string(event.object_id) + "/" +
                                std::to_string(profile.allocations.size());
                allocation.process_id = event.process_id;
                allocation.size_bytes = event.size_bytes;
                allocation.stack_frame_id = event.stack_frame_id;
                allocation.type_id = event.process_id + "/" + event.type_id;
                allocation.kind = AllocationKind::Managed;
                allocation.allocated_ts = event.ts_us;
                const size_t index = profile.allocations.size();
                if (!process.allocation_ids.insert(event.object_id).second) process.valid = false;
                process.live.emplace(event.object_id, index);
                profile.allocations.push_back(std::move(allocation));
                break;
            }
            case ManagedAllocationEventKind::GcStart:
                process.gc_instances.insert(event.clr_instance);
                if (process.gc || event.gc_type != 0 || event.gc_depth > 2) process.valid = false;
                if (process.last_gc_number && uint64_t(event.gc_number) != uint64_t(*process.last_gc_number) + 1)
                    process.valid = false;
                process.last_gc_number = event.gc_number;
                process.gc = &event;
                process.collected.clear();
                break;
            case ManagedAllocationEventKind::Collected:
                if (!process.gc) process.valid = false;
                process.collected.insert(process.collected.end(), event.collected_ids.begin(),
                                         event.collected_ids.end());
                break;
            case ManagedAllocationEventKind::GcEnd: {
                process.gc_instances.insert(event.clr_instance);
                if (!process.gc || process.gc->gc_number != event.gc_number ||
                    process.gc->clr_instance != event.clr_instance || process.gc->gc_depth != event.gc_depth) {
                    process.valid = false;
                    process.gc = nullptr;
                    break;
                }
                for (uint64_t id : process.collected) {
                    auto allocation = process.live.find(id);
                    if (allocation == process.live.end()) {
                        process.valid = false;
                        continue;
                    }
                    process.observations.push_back(
                        {profile.allocations[allocation->second].id, event.ts_us, false, {}});
                    process.live.erase(allocation);
                }
                // Positive observations require a complete blocking full collection.
                if (event.gc_depth == 2) {
                    for (const auto& [id, index] : process.live)
                        process.observations.push_back({profile.allocations[index].id, event.ts_us, true, {}});
                }
                process.gc = nullptr;
                process.collected.clear();
                break;
            }
        }
    }
    for (const auto& [id, process] : processes) {
        if (process.allocation_ids.empty()) continue;
        profile.capabilities.managed_allocation_history = true;
        profile.quality.incomplete_capture = true;
        // Sampling configuration is not supplied by the supported resource schema.
        warn(profile, "Managed totals describe recorded allocations; the capture's sampling rate is unavailable");
        bool desktop_clr = process.runtimes.size() == 1 && std::get<1>(*process.runtimes.begin()) == 1 &&
                           std::get<2>(*process.runtimes.begin()) == 4;
        if (!desktop_clr) {
            warn(profile,
                 "Managed GC survival is supported only for one Desktop CLR 4 runtime per process; "
                 "CoreCLR, unknown and mixed-runtime collection records are not used");
            continue;
        }
        const bool same_instance =
            process.gc_instances.empty() || (process.gc_instances.size() == 1 &&
                                             *process.gc_instances.begin() == std::get<0>(*process.runtimes.begin()));
        if (!complete_event_stream || unknown_process_gap || !process.valid || process.gc || !same_instance) {
            warn(profile, "Incomplete or ambiguous managed collection history; survival observations are unavailable");
            continue;
        }
        if (!process.observations.empty()) profile.capabilities.managed_survival = true;
        profile.managed_survival.insert(profile.managed_survival.end(), process.observations.begin(),
                                        process.observations.end());
    }
}
