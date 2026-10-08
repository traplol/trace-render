#include "memory_index.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr size_t BLOCK_SIZE = 256;
constexpr double INFINITY_TS = std::numeric_limits<double>::infinity();

bool add_amount(OutstandingMemoryAmount& total, uint64_t bytes, bool known) {
    auto& amount = known ? total.known : total.uncertain;
    const auto limit = std::numeric_limits<uint64_t>::max();
    if (amount.bytes > limit - bytes || amount.count == limit) return false;
    amount.bytes += bytes;
    ++amount.count;
    return total.known.bytes <= limit - total.uncertain.bytes && total.known.count <= limit - total.uncertain.count;
}

OutstandingMemory failure(const std::string& error) {
    OutstandingMemory result;
    result.error = error;
    return result;
}
}  // namespace

void MemoryIndex::clear() {
    entries_.clear();
    blocks_.clear();
    frame_functions_.clear();
    stack_functions_.clear();
}

void MemoryIndex::build(const ProfileData& profile, const std::vector<StackFrame>& frames,
                        const std::vector<std::string>& strings) {
    clear();
    if (!profile.capabilities.native_allocation_history) return;
    std::unordered_map<std::string, int32_t> frame_ids;
    std::map<std::tuple<int, uint32_t, uint32_t, uint64_t>, int32_t> functions;
    frame_functions_.resize(frames.size(), -1);
    stack_functions_.resize(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        if (!frame.valid || frame.id_idx >= strings.size()) continue;
        frame_ids[strings[frame.id_idx]] = static_cast<int32_t>(i);
        auto key = std::make_tuple(2, frame.module_id, frame.id_idx, UINT64_C(0));
        if (frame.symbol_id != UINT32_MAX)
            key = {0, frame.module_id, frame.symbol_id, 0};
        else if (frame.module_id != UINT32_MAX && frame.address)
            key = {1, frame.module_id, 0, *frame.address};
        frame_functions_[i] = functions.emplace(key, static_cast<int32_t>(i)).first->second;
    }
    std::unordered_map<std::string, std::optional<double>> heap_ends, process_ends;
    for (const auto& heap : profile.heaps) heap_ends.emplace(heap.id, heap.end_ts);
    for (const auto& process : profile.processes) process_ends.emplace(process.id, process.end_ts);
    for (size_t i = 0; i < profile.allocations.size(); ++i) {
        const auto& allocation = profile.allocations[i];
        if (allocation.kind != AllocationKind::Native) continue;
        Entry entry;
        entry.allocation = i;
        entry.start = allocation.allocated_ts.value_or(-INFINITY_TS);
        entry.end = allocation.freed_ts.value_or(INFINITY_TS);
        auto heap = heap_ends.find(allocation.heap_id);
        if (heap != heap_ends.end() && heap->second) entry.end = std::min(entry.end, *heap->second);
        auto process = process_ends.find(allocation.process_id);
        if (process != process_ends.end() && process->second) entry.end = std::min(entry.end, *process->second);
        auto frame = frame_ids.find(allocation.stack_frame_id);
        if (frame != frame_ids.end()) {
            entry.frame = frame->second;
            auto& stack = stack_functions_[entry.frame];
            if (stack.empty()) {
                std::unordered_set<int32_t> seen;
                int32_t current = entry.frame;
                while (current >= 0) {
                    int32_t function = frame_functions_[current];
                    if (seen.insert(function).second) stack.push_back(function);
                    current = frames[current].parent_idx;
                }
            }
        }
        entries_.push_back(entry);
    }
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.start < b.start; });
    for (size_t begin = 0; begin < entries_.size(); begin += BLOCK_SIZE) {
        Block block{begin, std::min(begin + BLOCK_SIZE, entries_.size()), -INFINITY_TS};
        for (size_t i = begin; i < block.end; ++i) block.max_end = std::max(block.max_end, entries_[i].end);
        blocks_.push_back(block);
    }
}

OutstandingMemory MemoryIndex::query(const ProfileData& profile, double ts,
                                     std::optional<std::pair<double, double>> born_between,
                                     const std::string& process_id) const {
    if (!profile.capabilities.native_allocation_history) return failure("Native allocation history is unavailable.");
    if (!std::isfinite(ts)) return failure("Outstanding memory requires a finite timestamp.");
    if ((profile.capture_start_ts && ts < *profile.capture_start_ts) ||
        (profile.capture_end_ts && ts > *profile.capture_end_ts))
        return failure("Outstanding memory is unavailable outside the recorded capture interval.");
    if (born_between && (!std::isfinite(born_between->first) || !std::isfinite(born_between->second) ||
                         born_between->first > born_between->second))
        return failure("Allocation birth range must have finite, ordered endpoints.");

    OutstandingMemory result;
    result.incomplete = profile.quality.incomplete_capture;
    const bool uncertain_coverage = profile.quality.allocation_history_gaps ||
                                    profile.quality.lost_events.value_or(0) > 0 || profile.quality.sampled_allocations;
    result.incomplete |= uncertain_coverage;
    std::map<std::pair<std::string, int32_t>, size_t> stacks, functions;
    for (const auto& block : blocks_) {
        if (entries_[block.begin].start > ts) break;
        if (block.max_end <= ts) continue;
        for (size_t i = block.begin; i < block.end; ++i) {
            const auto& entry = entries_[i];
            if (entry.start > ts) break;
            if (entry.end <= ts) continue;
            const auto& allocation = profile.allocations[entry.allocation];
            if (!process_id.empty() && allocation.process_id != process_id) continue;
            if (born_between) {
                if (!allocation.allocated_ts) {
                    ++result.unknown_birth_count;
                    result.incomplete = true;
                    continue;
                }
                if (*allocation.allocated_ts < born_between->first || *allocation.allocated_ts >= born_between->second)
                    continue;
            }
            const bool known = !uncertain_coverage && allocation.allocated_ts &&
                               ((allocation.end_state == AllocationEnd::Freed && allocation.freed_ts) ||
                                (allocation.end_state == AllocationEnd::LiveAtCaptureEnd && profile.capture_end_ts));
            result.incomplete |= !known;
            if (!add_amount(result.total, allocation.size_bytes, known))
                return failure("Outstanding allocation totals exceed the 64-bit range.");

            auto key = std::make_pair(allocation.process_id, entry.frame);
            auto stack = stacks.emplace(key, result.stacks.size());
            if (stack.second) result.stacks.push_back({allocation.process_id, entry.frame, {}});
            if (!add_amount(result.stacks[stack.first->second].total, allocation.size_bytes, known))
                return failure("Outstanding stack totals exceed the 64-bit range.");

            auto add_function = [&](int32_t frame, bool exclusive) {
                auto function =
                    functions.emplace(std::make_pair(allocation.process_id, frame), result.functions.size());
                if (function.second) result.functions.push_back({allocation.process_id, frame, {}, {}});
                auto& total = result.functions[function.first->second];
                return add_amount(total.inclusive, allocation.size_bytes, known) &&
                       (!exclusive || add_amount(total.exclusive, allocation.size_bytes, known));
            };
            if (entry.frame < 0) {
                if (!add_function(-1, true)) return failure("Outstanding function totals exceed the 64-bit range.");
            } else {
                for (int32_t frame : stack_functions_[entry.frame])
                    if (!add_function(frame, frame == frame_functions_[entry.frame]))
                        return failure("Outstanding function totals exceed the 64-bit range.");
            }
        }
    }
    return result;
}
