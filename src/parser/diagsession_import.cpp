#include "diagsession_import.h"
#include "diagsession_container.h"
#include "managed_allocations.h"
#include "managed_methods.h"
#include "managed_snapshot.h"
#include "native_heap.h"
#include "symbols/native_symbol_resolver.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
using json = nlohmann::json;
using StackKey = std::tuple<uint64_t, uint32_t, uint32_t>;

std::string hex(uint64_t value) {
    std::ostringstream s;
    s << "0x" << std::hex << value;
    return s.str();
}
std::string basename(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
void warning(ProfileData& profile, const std::string& message) {
    auto& warnings = profile.quality.warnings;
    if (std::find(warnings.begin(), warnings.end(), message) == warnings.end()) warnings.push_back(message);
}

struct LifecycleRecord {
    uint64_t qpc = 0, key = 0;
    uint32_t pid = 0, tid = 0;
    uint8_t opcode = 0;
    std::string name;
};
struct Lifetime {
    std::string id;
    uint32_t pid = 0, tid = 0;
    uint64_t key = 0, first_seen = 0;
    std::optional<uint64_t> start, end;
    std::string name;
};
struct ModuleRecord {
    uint64_t qpc = 0, base = 0, size = 0;
    uint32_t pid = 0;
    uint8_t opcode = 0;
    std::string path;
};
struct ModuleLifetime {
    size_t profile_index = 0;
    std::optional<uint64_t> start, end;
};
struct RsdsRecord {
    uint64_t qpc = 0, base = 0;
    uint32_t pid = 0;
    std::string build_id, path;
};
struct SampleRecord {
    uint64_t qpc = 0, ip = 0;
    uint32_t tid = 0;
    uint16_t count = 0, flags = 0;
    size_t source_index = 0;
};
struct RawHeapRecord {
    uint32_t pid = 0;
    NativeHeapEvent event;
};
struct IntervalRecord {
    uint64_t qpc = 0;
    uint32_t interval = 0;
};

class SessionBuilder {
public:
    SessionBuilder(TraceModel& model, ProfileData& profile) : model_(model), profile_(profile) {}

    void begin_resource(const std::string& name) { source_names_.push_back(name); }
    void end_resource(const EtlFileInfo& info) {
        lost_events_ += info.lost_events;
        lost_buffers_ |= info.lost_buffers != 0 || info.buffer_loss_flag;
        min_start_qpc_ = std::min(min_start_qpc_, info.start_qpc);
        end_filetime_ = std::max(end_filetime_, info.end_filetime);
    }

    void consume(const EtlFileInfo& info, const EtlRecord& r) {
        if (!info_.qpc_frequency) {
            info_ = info;
            min_start_qpc_ = info.start_qpc;
        } else if (info_.qpc_frequency != info.qpc_frequency || info_.boot_filetime != info.boot_filetime) {
            throw std::runtime_error("ETL resources do not share the same QPC clock and Windows boot");
        }
        EtlBytes p(r.payload);
        uint32_t ptr = r.pointer_size;
        if (managed_methods_.consume(r)) return;
        ManagedAllocationEvent managed;
        if (decode_managed_allocation(r, managed)) {
            managed_records_.push_back(std::move(managed));
            return;
        }
        const bool heap_provider = r.group == 16 || r.provider == "222962ab-6180-4b88-a825-346b75f2a24a";
        if (heap_provider && r.event_id >= 32 && r.event_id <= 36 && (r.opcode < 32 || r.opcode > 36)) {
            unsupported_heap_events_ = true;
            unsupported("manifest native heap event " + std::to_string(r.event_id), r.version);
            return;
        }
        if (r.extended_data) {
            ++extended_records_;
            if (heap_provider) unsupported_heap_events_ = true;
            return;
        }
        if (r.group == 15 && r.opcode == 46) {
            if (r.version != 2) {
                unsupported("CPU sample", r.version);
                return;
            }
            samples_.push_back(
                {r.qpc, p.pointer(0, ptr), p.u32(ptr), p.u16(ptr + 4), p.u16(ptr + 6), source_names_.size() - 1});
        } else if (r.group == 24 && r.opcode == 32) {
            if (r.version != 2) {
                unsupported("StackWalk", r.version);
                return;
            }
            if (r.payload.size() < 16 || (r.payload.size() - 16) % ptr)
                throw std::runtime_error("Invalid ETL StackWalk size");
            std::vector<uint64_t> frames;
            for (size_t at = 16; at < r.payload.size(); at += ptr) frames.push_back(p.pointer(at, ptr));
            stacks_[{p.u64(0), p.u32(8), p.u32(12)}].push_back(std::move(frames));
        } else if (r.group == 3 && r.opcode >= 1 && r.opcode <= 4) {
            if (r.version != 4 && r.version != 3) {
                unsupported("Process", r.version);
                return;
            }
            size_t sid = 20 + 2 * ptr;
            if (r.version == 3) sid -= 4;
            size_t name = sid + 4;
            if (p.u32(sid)) {
                size_t sid_start = sid + 2 * ptr;
                name = sid_start + 8 + p.u8(sid_start + 1) * 4;
            }
            process_records_.push_back({r.qpc, p.pointer(0, ptr), p.u32(ptr), 0, r.opcode, p.utf8(name)});
        } else if (r.group == 5 && r.opcode >= 1 && r.opcode <= 4) {
            if (r.version != 3 && r.version != 4 && r.version != 2) {
                unsupported("Thread", r.version);
                return;
            }
            size_t name_at = 16 + 7 * ptr;
            std::string name = r.payload.size() > name_at ? p.utf16(name_at) : "";
            thread_records_.push_back({r.qpc, 0, p.u32(0), p.u32(4), r.opcode, std::move(name)});
        } else if (r.group == 5 && r.opcode == 72 && r.version == 2) {
            thread_records_.push_back({r.qpc, 0, p.u32(0), p.u32(4), r.opcode, p.utf16(8)});
        } else if ((r.group == 3 && r.opcode == 10) || (r.group == 20 && r.opcode >= 2 && r.opcode <= 4)) {
            if (r.version != 2 && r.version != 3) {
                unsupported("Image", r.version);
                return;
            }
            module_records_.push_back(
                {r.qpc, p.pointer(0, ptr), p.pointer(ptr, ptr), p.u32(2 * ptr), r.opcode, p.utf16(32 + 3 * ptr)});
        } else if (r.provider == "b3e675d7-2554-4f18-830b-2762732560de" && r.opcode == 36) {
            if (r.version != 2) {
                unsupported("RSDS", r.version);
                return;
            }
            rsds_.push_back({r.qpc, p.pointer(0, ptr), p.u32(ptr),
                             p.guid(ptr + 4) + "/" + std::to_string(p.u32(ptr + 20)), p.utf8(ptr + 24)});
        } else if (r.group == 15 && r.opcode >= 72 && r.opcode <= 74) {
            if (r.version != 2 && r.version != 3) {
                unsupported("sampling interval", r.version);
                return;
            }
            if (p.u32(0) == 0) intervals_.push_back({r.qpc, r.opcode == 74 ? 0 : p.u32(4)});  // Timer, 100 ns.
        } else if (heap_provider && r.opcode >= 32 && r.opcode <= 36) {
            const size_t expected = r.opcode == 33                     ? 28
                                    : r.opcode == 34                   ? 44
                                    : r.opcode == 36                   ? 20
                                    : r.opcode == 32 && r.version == 3 ? 36
                                    : r.opcode == 32                   ? 12
                                                                       : 8;
            if (ptr != 8 || !r.pid || !r.tid || (r.version != 2 && !(r.opcode == 32 && r.version == 3))) {
                unsupported_heap_events_ = true;
                unsupported("native heap event", r.version);
                return;
            }
            if (r.payload.size() != expected) throw std::runtime_error("Invalid native heap event payload size");
            NativeHeapEvent heap;
            heap.raw_qpc = r.qpc;
            heap.tid = *r.tid;
            heap.heap_handle = p.u64(0);
            switch (r.opcode) {
                case 32:
                    heap.kind = NativeHeapEventKind::Create;
                    break;
                case 33:
                    heap.kind = NativeHeapEventKind::Allocate;
                    heap.size_bytes = p.u64(8);
                    heap.address = p.u64(16);
                    break;
                case 34:
                    heap.kind = NativeHeapEventKind::Reallocate;
                    heap.address = p.u64(8);
                    heap.old_address = p.u64(16);
                    heap.size_bytes = p.u64(24);
                    heap.old_size_bytes = p.u64(32);
                    break;
                case 35:
                    heap.kind = NativeHeapEventKind::Destroy;
                    break;
                case 36:
                    heap.kind = NativeHeapEventKind::Free;
                    heap.address = p.u64(8);
                    break;
            }
            heap_records_.push_back({*r.pid, std::move(heap)});
        }
    }

    void finish(const ImportProgress& progress) {
        auto start = timestamp(min_start_qpc_);
        auto end = static_cast<double>((static_cast<long double>(end_filetime_) - info_.start_filetime) / 10);
        if (end_filetime_ < info_.start_filetime) throw std::runtime_error("ETL capture ends before it starts");
        profile_.capture_start_ts = start;
        profile_.capture_end_ts = end;
        profile_.quality.lost_events = lost_events_;
        if (lost_events_ || lost_buffers_) {
            profile_.quality.incomplete_capture = true;
            warning(profile_, "ETL reports lost events or buffers; CPU observations are incomplete");
        }
        if (extended_records_)
            warning(profile_, "Some ETL events with EventHeader extensions are not used by this importer");
        build_lifetimes(process_records_, processes_, process_ids_, false);
        build_lifetimes(thread_records_, threads_, thread_ids_, true);
        for (const auto& life : processes_) add_profile_process(life);
        build_modules();
        managed_methods_.build([&](uint32_t pid, uint64_t qpc) { return process_at(pid, qpc); });
        for (const auto& message : managed_methods_.warnings()) warning(profile_, message);
        std::sort(intervals_.begin(), intervals_.end(), [](auto& a, auto& b) { return a.qpc < b.qpc; });
        std::stable_sort(samples_.begin(), samples_.end(), [](auto& a, auto& b) { return a.qpc < b.qpc; });
        // StackWalk carries a PID while SampleProf carries only a TID.
        std::map<std::pair<uint64_t, uint32_t>, std::optional<uint32_t>> stack_pids;
        for (const auto& [key, parts] : stacks_) {
            auto [qpc, pid, tid] = key;
            auto inserted = stack_pids.emplace(std::make_pair(qpc, tid), pid);
            if (!inserted.second && inserted.first->second != pid) inserted.first->second.reset();
        }
        size_t missing_stacks = 0, unknown_processes = 0;
        std::set<StackKey> used_stacks;
        for (size_t i = 0; i < samples_.size(); ++i) {
            if ((i % 4096) == 0 && progress && !progress("Converting CPU samples", float(i) / samples_.size()))
                throw std::runtime_error("Import canceled");
            const auto& s = samples_[i];
            auto* thread = lifetime_at(threads_, thread_ids_, s.tid, s.qpc);
            std::optional<uint32_t> pid = thread ? std::make_optional(thread->pid) : std::nullopt;
            auto sp = stack_pids.find({s.qpc, s.tid});
            if (sp != stack_pids.end() && sp->second) pid = *sp->second;
            if (!pid) ++unknown_processes;
            auto process_id = process_at(pid, s.qpc, s.tid);
            json args = {{"raw_qpc", s.qpc},
                         {"raw_sample_ip", s.ip},
                         {"sample_count", s.count},
                         {"sample_flags", s.flags},
                         {"qpc_frequency_hz", info_.qpc_frequency},
                         {"timestamp_precision", "QPC ticks"},
                         {"source_resource", source_names_[s.source_index]},
                         {"pid_known", pid.has_value()}};
            if (thread && (!pid || thread->pid == *pid)) {
                args["thread_instance_id"] = thread->id;
                if (thread->start) args["thread_start_qpc"] = *thread->start;
                if (thread->end) args["thread_end_qpc"] = *thread->end;
                if (!thread->name.empty())
                    model_.get_or_create_process(thread->pid).get_or_create_thread(s.tid).name = thread->name;
            }
            std::vector<uint64_t> frames;
            auto stack = stacks_.find({s.qpc, pid.value_or(0), s.tid});
            if (pid && stack != stacks_.end()) {
                used_stacks.insert(stack->first);
                args["raw_stack_parts"] = stack->second;
                frames = combine_stack(stack->second);
            } else {
                ++missing_stacks;
                frames.push_back(s.ip);
            }
            TraceEvent event;
            event.ph = Phase::Sample;
            event.kind = EventKind::Sample;
            event.ts = timestamp(s.qpc);
            event.pid = pid.value_or(0);
            event.tid = s.tid;
            event.process_instance_id = model_.intern_string(process_id);
            event.cat_idx = model_.intern_string("CPU samples");
            event.name_idx = model_.intern_string("CPU sample");
            event.sample_weight = 1;  // One recorded sample; retain the legacy Count field separately.
            event.sample_weight_unit = model_.intern_string("samples");
            auto interval = std::upper_bound(intervals_.begin(), intervals_.end(), s.qpc,
                                             [](auto qpc, const auto& item) { return qpc < item.qpc; });
            if (interval != intervals_.begin() && (--interval)->interval) {
                event.sample_cpu_time = interval->interval / 10.0;
                args["sampling_interval_100ns"] = interval->interval;
            }
            event.stack_frame_id = make_stack(process_id, s.qpc, frames, true);
            event.args_idx = model_.add_args(args.dump());
            model_.add_event(event);
        }
        if (!samples_.empty()) {
            profile_.capabilities.native_cpu_samples = true;
            profile_.quality.sampled_cpu = true;
            profile_.quality.unresolved_symbols = true;
        }
        if (missing_stacks)
            warning(profile_, std::to_string(missing_stacks) +
                                  " CPU samples have only the recorded instruction pointer; no matching StackWalk");
        if (unknown_processes)
            warning(profile_, std::to_string(unknown_processes) + " CPU samples have unknown process attribution");
        std::stable_sort(heap_records_.begin(), heap_records_.end(),
                         [](const auto& a, const auto& b) { return a.event.raw_qpc < b.event.raw_qpc; });
        std::vector<NativeHeapEvent> heaps;
        heaps.reserve(heap_records_.size());
        for (size_t i = 0; i < heap_records_.size(); ++i) {
            if ((i % 4096) == 0 && progress &&
                !progress("Converting native allocations", float(i) / heap_records_.size()))
                throw std::runtime_error("Import canceled");
            auto& raw = heap_records_[i];
            auto& event = raw.event;
            event.process_id = process_at(raw.pid, event.raw_qpc);
            event.ts_us = timestamp(event.raw_qpc);
            auto stack = stacks_.find({event.raw_qpc, raw.pid, event.tid});
            if (stack != stacks_.end()) {
                auto frame = make_stack(event.process_id, event.raw_qpc, combine_stack(stack->second));
                if (frame != UINT32_MAX) event.stack_frame_id = model_.get_string(frame);
                used_stacks.insert(stack->first);
            }
            heaps.push_back(std::move(event));
        }
        profile_.quality.allocation_history_gaps |= unsupported_heap_events_;
        build_native_allocations(heaps, profile_, !lost_events_ && !lost_buffers_ && !unsupported_heap_events_);
        for (size_t i = 0; i < managed_records_.size(); ++i) {
            if (i % 4096 == 0 && progress &&
                !progress("Resolving managed allocation stacks", float(i) / managed_records_.size()))
                throw std::runtime_error("Import canceled");
            auto& event = managed_records_[i];
            event.process_id = process_at(event.pid, event.raw_qpc);
            event.ts_us = timestamp(event.raw_qpc);
            if (!event.addresses.empty()) {
                auto frame = make_stack(event.process_id, event.raw_qpc, event.addresses);
                if (frame != UINT32_MAX) event.stack_frame_id = model_.get_string(frame);
            }
        }
        build_managed_allocations(managed_records_, profile_, !lost_events_ && !lost_buffers_, progress);
        if (used_stacks.size() < stacks_.size())
            warning(profile_, std::to_string(stacks_.size() - used_stacks.size()) +
                                  " StackWalk keys have no decoded CPU or allocation observation");
    }

private:
    double timestamp(uint64_t qpc) const {
        return static_cast<double>((static_cast<long double>(qpc) - info_.start_qpc) * 1000000 / info_.qpc_frequency);
    }
    void unsupported(const std::string& kind, uint16_t version) {
        profile_.quality.incomplete_capture = true;
        warning(profile_, "Unsupported ETL " + kind + " version " + std::to_string(version));
    }
    void build_lifetimes(std::vector<LifecycleRecord>& records, std::vector<Lifetime>& lifetimes,
                         std::map<uint32_t, std::vector<size_t>>& by_id, bool thread) {
        std::stable_sort(records.begin(), records.end(), [](auto& a, auto& b) { return a.qpc < b.qpc; });
        for (const auto& r : records) {
            uint32_t id = thread ? r.tid : r.pid;
            auto& candidates = by_id[id];
            Lifetime* match = nullptr;
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                auto& life = lifetimes[*it];
                if (life.pid == r.pid && (!r.key || life.key == r.key) && !life.end) {
                    match = &life;
                    break;
                }
            }
            if (r.opcode == 1 && match && match->start && *match->start != r.qpc) {
                match->end = r.qpc;
                match = nullptr;
            }
            if (!match) {
                candidates.push_back(lifetimes.size());
                lifetimes.push_back(
                    {prefix_ + (thread ? "/t/" : "/p/") + std::to_string(id) + "/" + std::to_string(candidates.size()),
                     r.pid,
                     r.tid,
                     r.key,
                     r.qpc,
                     {},
                     {},
                     r.name});
                match = &lifetimes.back();
            }
            if (r.opcode == 1) match->start = r.qpc;
            if (r.opcode == 2) match->end = r.qpc;
            if (!r.name.empty()) match->name = r.name;
        }
    }
    Lifetime* lifetime_at(std::vector<Lifetime>& lives, const std::map<uint32_t, std::vector<size_t>>& ids, uint32_t id,
                          uint64_t qpc) {
        auto found = ids.find(id);
        if (found == ids.end()) return nullptr;
        Lifetime* best = nullptr;
        for (size_t index : found->second) {
            auto& life = lives[index];
            if ((life.start && qpc < *life.start) || (life.end && qpc >= *life.end)) continue;
            if (!best || (life.first_seen <= qpc && (best->first_seen > qpc || life.first_seen > best->first_seen)))
                best = &life;
        }
        return best;
    }
    void add_profile_process(const Lifetime& life) {
        profile_.processes.push_back({life.id, life.pid,
                                      life.start ? std::make_optional(timestamp(*life.start)) : std::nullopt,
                                      life.end ? std::make_optional(timestamp(*life.end)) : std::nullopt});
        if (!life.name.empty()) model_.get_or_create_process(life.pid).name = life.name;
    }
    std::string process_at(std::optional<uint32_t> pid, uint64_t qpc, uint32_t tid = 0) {
        if (pid)
            if (auto* life = lifetime_at(processes_, process_ids_, *pid, qpc)) return life->id;
        std::string id =
            prefix_ + (pid ? "/p/" + std::to_string(*pid) : "/unknown-thread/" + std::to_string(tid)) + "/unknown";
        if (unknown_process_ids_.insert(id).second) profile_.processes.push_back({id, pid.value_or(0), {}, {}});
        return id;
    }
    void build_modules() {
        std::stable_sort(module_records_.begin(), module_records_.end(),
                         [](auto& a, auto& b) { return a.qpc < b.qpc; });
        for (const auto& r : module_records_) {
            auto process_id = process_at(r.pid, r.qpc);
            auto& versions = modules_[process_id][r.base];
            ModuleLifetime* match = !versions.empty() && !versions.back().end ? &versions.back() : nullptr;
            if (r.opcode == 10 && match && match->start && *match->start != r.qpc) {
                match->end = r.qpc;
                profile_.modules[match->profile_index].unload_ts = timestamp(r.qpc);
                match = nullptr;
            }
            if (match && profile_.modules[match->profile_index].path != r.path) {
                match->end = r.qpc;
                profile_.modules[match->profile_index].unload_ts = timestamp(r.qpc);
                match = nullptr;
            }
            if (!match) {
                size_t idx = profile_.modules.size();
                profile_.modules.push_back({prefix_ + "/m/" + std::to_string(idx),
                                            process_id,
                                            basename(r.path),
                                            r.path,
                                            "",
                                            r.base,
                                            r.size,
                                            {},
                                            {}});
                versions.push_back({idx, {}, {}});
                match = &versions.back();
            }
            auto& module = profile_.modules[match->profile_index];
            if (r.opcode == 10) {
                match->start = r.qpc;
                module.load_ts = timestamp(r.qpc);
            }
            if (r.opcode == 2) {
                match->end = r.qpc;
                module.unload_ts = timestamp(r.qpc);
            }
        }
        std::set<std::string> conflicting_pdbs;
        for (const auto& r : rsds_) {
            auto* module = module_at(process_at(r.pid, r.qpc), r.base, r.qpc);
            if (module && module->load_address == r.base) {
                if (conflicting_pdbs.count(module->id)) continue;
                if (!module->build_id.empty() && module->build_id != r.build_id) {
                    warning(profile_, "Conflicting PDB identities for a module lifetime; symbols remain unresolved");
                    module->build_id.clear();
                    module->pdb_path.clear();
                    conflicting_pdbs.insert(module->id);
                } else {
                    module->build_id = r.build_id;
                    module->pdb_path = r.path;
                }
            }
        }
    }
    ProfileModule* module_at(const std::string& process_id, uint64_t address, uint64_t qpc) {
        auto proc = modules_.find(process_id);
        if (proc == modules_.end()) return nullptr;
        auto found = proc->second.upper_bound(address);
        if (found == proc->second.begin()) return nullptr;
        --found;
        for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
            auto& module = profile_.modules[it->profile_index];
            if ((!it->start || qpc >= *it->start) && (!it->end || qpc < *it->end) && address >= module.load_address &&
                address - module.load_address < module.size_bytes)
                return &module;
        }
        return nullptr;
    }
    std::vector<uint64_t> combine_stack(const std::vector<std::vector<uint64_t>>& parts) const {
        std::vector<uint64_t> frames;
        // Kernel StackWalk is the leaf half, user StackWalk the root half.
        // Preserve the original parts separately in args, including their order.
        uint64_t kernel_bit = info_.pointer_size == 8 ? uint64_t(1) << 63 : uint64_t(1) << 31;
        for (bool kernel : {true, false})
            for (const auto& part : parts)
                if (!part.empty() && bool(part.front() & kernel_bit) == kernel)
                    frames.insert(frames.end(), part.begin(), part.end());
        return frames;
    }
    uint32_t make_stack(const std::string& process_id, uint64_t qpc, const std::vector<uint64_t>& frames,
                        bool cpu = false) {
        uint32_t parent = UINT32_MAX;
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            auto* managed = managed_methods_.find(process_id, *it, qpc);
            if (managed && cpu) profile_.capabilities.managed_cpu_samples = true;
            auto* module = module_at(process_id, *it, qpc);
            if (!module && (*it & (info_.pointer_size == 8 ? uint64_t(1) << 63 : uint64_t(1) << 31))) {
                if (auto* system = lifetime_at(processes_, process_ids_, 0, qpc))
                    module = module_at(system->id, *it, qpc);
            }
            std::string module_id = module ? module->id : process_id;
            auto key = std::make_tuple(managed ? managed->symbol_id : module_id, *it, parent);
            auto found = frame_ids_.find(key);
            if (found != frame_ids_.end()) {
                parent = found->second;
                continue;
            }
            StackFrame frame;
            frame.id_idx = model_.intern_string(prefix_ + "/sf/" + std::to_string(frame_ids_.size()));
            frame.parent_id = parent;
            frame.address = *it;
            if (managed) {
                frame.symbol_id = model_.intern_string(managed->symbol_id);
                frame.cat_idx = model_.intern_string("Managed");
                frame.symbol_resolved = !managed->name.empty();
                frame.name_idx = model_.intern_string(frame.symbol_resolved ? managed->name
                                                                            : "CLR method " + hex(managed->method_id) +
                                                                                  "+" + hex(*it - managed->address));
            } else {
                frame.symbol_id = model_.intern_string(module_id + "/ip/" + hex(*it));
                frame.cat_idx = model_.intern_string(module ? "Native" : "Unresolved");
                frame.name_idx =
                    model_.intern_string(module ? module->name + "+" + hex(*it - module->load_address) : hex(*it));
                if (module) frame.module_id = model_.intern_string(module->id);
            }
            model_.add_stack_frame(frame);
            profile_.quality.unresolved_symbols = true;
            parent = frame.id_idx;
            frame_ids_.emplace(std::move(key), parent);
        }
        return parent;
    }

    const std::string prefix_ = "session";
    std::vector<std::string> source_names_;
    TraceModel& model_;
    ProfileData& profile_;
    ManagedMethods managed_methods_;
    EtlFileInfo info_;
    uint64_t min_start_qpc_ = 0, end_filetime_ = 0, lost_events_ = 0;
    bool lost_buffers_ = false, unsupported_heap_events_ = false;
    std::vector<RawHeapRecord> heap_records_;
    std::vector<ManagedAllocationEvent> managed_records_;
    size_t extended_records_ = 0;
    std::vector<LifecycleRecord> process_records_, thread_records_;
    std::vector<Lifetime> processes_, threads_;
    std::map<uint32_t, std::vector<size_t>> process_ids_, thread_ids_;
    std::vector<ModuleRecord> module_records_;
    std::vector<RsdsRecord> rsds_;
    std::vector<SampleRecord> samples_;
    std::vector<IntervalRecord> intervals_;
    std::map<StackKey, std::vector<std::vector<uint64_t>>> stacks_;
    std::map<std::string, std::map<uint64_t, std::vector<ModuleLifetime>>> modules_;
    std::set<std::string> unknown_process_ids_;
    std::map<std::tuple<std::string, uint64_t, uint32_t>, uint32_t> frame_ids_;
};
}  // namespace

bool is_diagsession_container(std::string_view bytes) {
    return bytes.substr(0, 2) == "PK" || bytes.substr(0, 8) == std::string_view("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8);
}

bool read_diagsession(std::string_view bytes, TraceModel& model, std::string& error, const ImportProgress& progress,
                      NativeSymbolResolver* symbols) {
    model.clear();
    error.clear();
    try {
        DiagsessionContainer container;
        if (!container.open(bytes, error)) return false;
        TraceModel result;
        result.intern_string("");
        ProfileData profile;
        profile.source_format = "diagsession";
        profile.converter = "TraceRender diagsession v1";
        size_t etls = 0, ignored = 0;
        SessionBuilder builder(result, profile);
        for (size_t i = 0; i < container.resources().size(); ++i) {
            const auto& resource = container.resources()[i];
            if (progress && !progress("Reading diagsession resources", float(i) / container.resources().size()))
                throw std::runtime_error("Import canceled");
            if (resource.directory) continue;
            if (symbols && resource.type == "DiagnosticsHub.Resource.EmbeddedPdbs") {
                std::vector<uint8_t> data;
                std::string symbol_error;
                auto cancelled = [&] {
                    return progress && !progress("Reading embedded symbols", float(i) / container.resources().size());
                };
                if (!container.read_resource(i, data, symbol_error, cancelled)) {
                    warning(profile, "Embedded symbols could not be read: " + symbol_error);
                    continue;
                }
                const std::string_view bytes(reinterpret_cast<const char*>(data.data()), data.size());
                constexpr std::string_view magic = "Microsoft C/C++ MSF";
                if (bytes.substr(0, magic.size()) != magic) {
                    ++ignored;
                    continue;
                }
                if (!symbols->add_embedded_pdb(resource.stored_path, bytes, symbol_error))
                    warning(profile, "Embedded native symbols could not be read: " + symbol_error);
                continue;
            }
            if (resource.type != "DiagnosticsHub.Resource.EtlFile") {
                if (resource.type != "MemoryProfiler.Manifest" && resource.type != "MemoryProfiler.GCDump") ++ignored;
                continue;
            }
            std::vector<uint8_t> data;
            auto canceled = [&] {
                return progress && !progress("Extracting ETL resource", float(i) / container.resources().size());
            };
            if (!container.read_resource(i, data, error, canceled)) return false;
            builder.begin_resource(resource.name);
            EtlFileInfo info;
            if (!read_etl(
                    {reinterpret_cast<const char*>(data.data()), data.size()}, info,
                    [&](const auto& file, const auto& event) {
                        builder.consume(file, event);
                        return true;
                    },
                    error, progress)) {
                error = resource.name + ": " + error;
                return false;
            }
            builder.end_resource(info);
            ++etls;
        }
        if (etls) builder.finish(progress);
        if (!read_managed_snapshots(container, profile, error, progress)) return false;
        if (!etls && !profile.capabilities.managed_heap_snapshots)
            throw std::runtime_error(
                "Diagsession contains no supported ETL resource or managed heap snapshot referenced by metadata");
        if (!profile.capabilities.native_cpu_samples && !profile.capabilities.native_allocation_history &&
            !profile.capabilities.managed_allocation_history)
            warning(profile, "No supported CPU samples or allocation history were found in ETL resources");
        if (ignored) warning(profile, "Some diagsession resources are not decoded by this importer");
        result.set_profile(std::move(profile));
        if (progress && !progress("Building profile index", 0)) throw std::runtime_error("Import canceled");
        result.build_index();
        model = std::move(result);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
