#include "managed_methods.h"
#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace {
// Primary schemas: dotnet/runtime src/coreclr/vm/ClrEtwAll.man and
// microsoft/perfview src/TraceEvent/Parsers/ClrTraceEventParser.cs.
constexpr const char* runtime_provider = "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";
constexpr const char* rundown_provider = "a669021c-c450-4609-a035-5af59af4df18";

std::string next_string(const EtlBytes& bytes, size_t& at) {
    auto result = bytes.utf16(at);
    while (bytes.u16(at)) at += 2;
    at += 2;
    return result;
}
void constrain_end(ManagedMethod& method, uint64_t qpc) {
    if (!method.end_qpc || qpc < *method.end_qpc) method.end_qpc = qpc;
}
void constrain_start(ManagedMethod& method, uint64_t qpc) {
    if (!method.start_qpc || qpc > *method.start_qpc) method.start_qpc = qpc;
}
bool times_overlap(const ManagedMethod& a, const ManagedMethod& b) {
    return std::max(a.start_qpc.value_or(0), b.start_qpc.value_or(0)) <
           std::min(a.end_qpc.value_or(UINT64_MAX), b.end_qpc.value_or(UINT64_MAX));
}
}  // namespace

void ManagedMethods::warn(const std::string& message) {
    if (std::find(warnings_.begin(), warnings_.end(), message) == warnings_.end()) warnings_.push_back(message);
}

bool ManagedMethods::consume(const EtlRecord& r) {
    const bool rundown = r.provider == rundown_provider;
    if (!rundown && r.provider != runtime_provider) return false;
    Kind kind = Kind::Rundown;
    bool verbose = false;
    if (rundown) {
        if (r.event_id < 141 || r.event_id > 144) return false;
        verbose = r.event_id >= 143;
    } else if (r.event_id >= 137 && r.event_id <= 140) {
        verbose = r.event_id >= 139;
    } else if (r.event_id >= 141 && r.event_id <= 144) {
        kind = r.event_id == 141 || r.event_id == 143 ? Kind::Load : Kind::Unload;
        verbose = r.event_id >= 143;
    } else {
        return false;
    }
    if (r.extended_data || r.version > 2 || !r.pid) {
        if (r.pid) gaps_.push_back({*r.pid, r.qpc, kind == Kind::Rundown});
        warn("Unsupported CLR method records were skipped; affected addresses remain unresolved");
        return true;
    }
    try {
        EtlBytes p(r.payload);
        ManagedMethod method;
        method.method_id = p.u64(0);
        method.module_id = p.u64(8);
        method.address = p.u64(16);
        method.size = p.u32(24);
        method.flags = p.u32(32);
        size_t at = 36;
        if (verbose) {
            auto ns = next_string(p, at);
            auto name = next_string(p, at);
            auto signature = next_string(p, at);
            if (!name.empty()) {
                method.name = ns.empty() ? name : ns + "." + name;
                auto args = signature.find('(');
                if (args != std::string::npos) method.name += signature.substr(args);
            }
        }
        if (r.version >= 1) {
            method.clr_instance = p.u16(at);
            at += 2;
        }
        if (r.version >= 2) {
            method.rejit_id = p.u64(at);
            at += 8;
        }
        if (at != r.payload.size()) throw std::runtime_error("CLR method payload length");
        if (!method.address || !method.size || method.size > UINT64_MAX - method.address)
            throw std::runtime_error("CLR method code range");
        records_.push_back({std::move(method), r.qpc, *r.pid, kind});
    } catch (const std::exception&) {
        gaps_.push_back({*r.pid, r.qpc, kind == Kind::Rundown});
        warn("Malformed CLR method metadata was skipped; CPU observations are preserved");
    }
    return true;
}

void ManagedMethods::build(const std::function<std::string(uint32_t, uint64_t)>& process_at) {
    methods_.clear();
    ranges_.clear();
    process_gaps_.clear();
    for (const auto& gap : gaps_) process_gaps_[process_at(gap.pid, gap.qpc)].push_back(gap.rundown ? 0 : gap.qpc);
    for (auto& [process, gaps] : process_gaps_) std::sort(gaps.begin(), gaps.end());
    std::stable_sort(records_.begin(), records_.end(), [](const auto& a, const auto& b) { return a.qpc < b.qpc; });
    using Key = std::tuple<std::string, uint16_t, uint64_t, uint64_t, uint64_t, uint32_t, uint64_t, uint64_t>;
    std::map<Key, size_t> latest;
    bool rundown_used = false;
    for (const auto& r : records_) {
        auto process = process_at(r.pid, r.qpc);
        const auto& m = r.method;
        Key key{process, m.clr_instance, m.module_id, m.method_id, m.rejit_id, m.flags >> 28, m.address, m.size};
        auto found = latest.find(key);
        Lifetime* life = found == latest.end() ? nullptr : &methods_[found->second];
        if (life && life->method.end_qpc && !(r.kind == Kind::Unload && *life->method.end_qpc == r.qpc)) life = nullptr;
        if (life && r.kind == Kind::Load && life->first_seen != r.qpc) {
            constrain_end(life->method, r.qpc);
            life = nullptr;
        }
        if (!life) {
            auto previous_end =
                found == latest.end() ? std::optional<uint64_t>{} : methods_[found->second].method.end_qpc;
            latest[key] = methods_.size();
            methods_.push_back({m, r.qpc, r.qpc, r.kind == Kind::Load, false});
            life = &methods_.back();
            life->method.process_id = process;
            life->method.symbol_id = process + "/clr/" + std::to_string(methods_.size());
            if (previous_end) life->method.start_qpc = r.qpc;
        }
        life->last_seen = r.qpc;
        if (!life->conflicting_name && !m.name.empty()) {
            if (!life->method.name.empty() && life->method.name != m.name) {
                life->conflicting_name = true;
                life->method.name.clear();
                warn("Conflicting CLR method names remain unresolved");
            } else {
                life->method.name = m.name;
            }
        }
        if (r.kind == Kind::Load) {
            life->method.start_qpc = r.qpc;
            life->load_observed = true;
        }
        if (r.kind == Kind::Unload) life->method.end_qpc = r.qpc;
        rundown_used |= r.kind == Kind::Rundown;
    }
    for (size_t i = 0; i < methods_.size(); ++i) ranges_[methods_[i].method.process_id].push_back({i, 0});
    for (auto& [process, ranges] : ranges_) {
        std::stable_sort(ranges.begin(), ranges.end(), [&](const auto& a, const auto& b) {
            return methods_[a.index].method.address < methods_[b.index].method.address;
        });
        uint64_t max_end = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            auto& current = methods_[ranges[i].index];
            auto base = current.method.address;
            for (size_t j = i; j > 0 && ranges[j - 1].max_end > base;) {
                auto& other = methods_[ranges[--j].index];
                if (other.method.address + other.method.size <= base || !times_overlap(current.method, other.method))
                    continue;
                auto* earlier = &other;
                auto* later = &current;
                if (earlier->first_seen > later->first_seen) std::swap(earlier, later);
                // Shared generic code can name several methods at once. An address
                // alone cannot choose one, so find() leaves that overlap unresolved.
                if (earlier->first_seen == later->first_seen || ((earlier->method.flags | later->method.flags) & 4))
                    continue;
                if (later->load_observed) {
                    constrain_end(earlier->method, *later->method.start_qpc);
                } else if (earlier->method.end_qpc && *earlier->method.end_qpc <= later->first_seen) {
                    constrain_start(later->method, *earlier->method.end_qpc);
                } else {
                    constrain_end(earlier->method, earlier->last_seen + (earlier->last_seen != UINT64_MAX));
                    constrain_start(later->method, later->first_seen);
                    warn(
                        "CLR rundown reveals code reuse without a recorded transition; the intervening addresses "
                        "remain unresolved");
                }
            }
            max_end = std::max(max_end, base + current.method.size);
            ranges[i].max_end = max_end;
        }
    }
    if (rundown_used)
        warn(
            "CLR rundown supplies code ranges without compilation timestamps; unrecorded code transitions cannot be "
            "recovered");
    if (!methods_.empty())
        warn("Managed source locations are unavailable without validated managed PDB and IL-to-native mappings");
}

const ManagedMethod* ManagedMethods::find(const std::string& process, uint64_t address, uint64_t qpc) const {
    auto found = ranges_.find(process);
    if (found == ranges_.end()) return nullptr;
    const auto& ranges = found->second;
    auto end = std::upper_bound(ranges.begin(), ranges.end(), address, [&](uint64_t value, const auto& range) {
        return value < methods_[range.index].method.address;
    });
    const ManagedMethod* result = nullptr;
    std::optional<uint64_t> last_gap;
    auto gaps = process_gaps_.find(process);
    if (gaps != process_gaps_.end()) {
        auto next = std::upper_bound(gaps->second.begin(), gaps->second.end(), qpc);
        if (next != gaps->second.begin()) last_gap = *--next;
    }
    while (end != ranges.begin()) {
        --end;
        if (end->max_end <= address) break;
        const auto& life = methods_[end->index];
        const auto& method = life.method;
        if (address - method.address >= method.size || (method.start_qpc && qpc < *method.start_qpc) ||
            (method.end_qpc && qpc >= *method.end_qpc))
            continue;
        if (last_gap && (!life.load_observed || !method.start_qpc || *method.start_qpc <= *last_gap)) return nullptr;
        if (result) return nullptr;
        result = &method;
    }
    return result;
}
