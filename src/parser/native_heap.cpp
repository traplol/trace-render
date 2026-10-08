#include "native_heap.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace {
struct HeapState {
    size_t index = 0;
    std::unordered_map<uint64_t, size_t> active;
};

void warning(ProfileData& profile, const std::string& text) {
    profile.quality.incomplete_capture = true;
    auto& warnings = profile.quality.warnings;
    if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text);
}

void release(AllocationLifetime& allocation, double ts) {
    allocation.freed_ts = ts;
    allocation.end_state = AllocationEnd::Freed;
}
}  // namespace

void build_native_allocations(const std::vector<NativeHeapEvent>& events, ProfileData& profile,
                              bool complete_event_stream) {
    if (events.empty()) return;
    profile.capabilities.native_allocation_history = true;
    const size_t first_allocation = profile.allocations.size();
    std::unordered_map<std::string, const ProfileProcess*> processes;
    for (const auto& process : profile.processes) processes.emplace(process.id, &process);
    std::unordered_map<std::string, std::unordered_map<uint64_t, HeapState>> process_heaps;
    bool missing_stack = false;
    double previous_ts = -std::numeric_limits<double>::infinity();

    if (!complete_event_stream)
        warning(profile, "Native heap events are incomplete; unmatched allocation ends remain unknown.");

    for (const auto& event : events) {
        if (!std::isfinite(event.ts_us) || event.ts_us < previous_ts) {
            warning(profile, "Invalid or out-of-order native heap events were omitted.");
            complete_event_stream = false;
            continue;
        }
        previous_ts = event.ts_us;
        if ((profile.capture_start_ts && event.ts_us < *profile.capture_start_ts) ||
            (profile.capture_end_ts && event.ts_us > *profile.capture_end_ts)) {
            warning(profile, "Native heap events outside the capture interval were omitted.");
            complete_event_stream = false;
            continue;
        }
        auto process_it = processes.find(event.process_id);
        if (process_it == processes.end()) {
            warning(profile, "Native heap events without a process lifetime were omitted.");
            continue;
        }
        const auto& process = *process_it->second;
        if ((process.start_ts && event.ts_us < *process.start_ts) ||
            (process.end_ts && event.ts_us > *process.end_ts)) {
            warning(profile, "Native heap events outside their process lifetime were omitted.");
            continue;
        }
        auto& heaps = process_heaps[event.process_id];
        auto heap_it = heaps.find(event.heap_handle);
        if (event.kind == NativeHeapEventKind::Create && heap_it != heaps.end()) {
            // A new heap with the same handle bounds the previous heap generation,
            // but does not supply an exact release timestamp for its allocations.
            profile.heaps[heap_it->second.index].end_ts = event.ts_us;
            warning(profile, "A native heap handle was reused without an observed destruction.");
            heaps.erase(heap_it);
            heap_it = heaps.end();
        }
        if (heap_it == heaps.end()) {
            ProfileHeap heap;
            heap.id = "native-heap-" + std::to_string(profile.heaps.size());
            heap.process_id = event.process_id;
            if (event.kind == NativeHeapEventKind::Create)
                heap.start_ts = event.ts_us;
            else
                warning(profile, "Some native heaps predate recording; initial heap contents are unknown.");
            size_t index = profile.heaps.size();
            profile.heaps.push_back(std::move(heap));
            heap_it = heaps.emplace(event.heap_handle, HeapState{index, {}}).first;
        }
        auto& heap = heap_it->second;
        auto add_allocation = [&](uint64_t address, uint64_t size) {
            auto active = heap.active.find(address);
            if (active != heap.active.end()) {
                warning(profile,
                        "Native allocation addresses were reused without observed frees; prior ends are unknown.");
                heap.active.erase(active);
            }
            AllocationLifetime allocation;
            allocation.id = "native-allocation-" + std::to_string(profile.allocations.size());
            allocation.process_id = event.process_id;
            allocation.heap_id = profile.heaps[heap.index].id;
            allocation.address = address;
            allocation.size_bytes = size;
            allocation.stack_frame_id = event.stack_frame_id;
            allocation.allocated_ts = event.ts_us;
            missing_stack |= allocation.stack_frame_id.empty();
            heap.active[address] = profile.allocations.size();
            profile.allocations.push_back(std::move(allocation));
        };

        switch (event.kind) {
            case NativeHeapEventKind::Create:
                break;
            case NativeHeapEventKind::Allocate:
                if (event.address != 0) add_allocation(event.address, event.size_bytes);
                break;
            case NativeHeapEventKind::Free: {
                if (event.address == 0) break;
                auto active = heap.active.find(event.address);
                if (active != heap.active.end()) {
                    release(profile.allocations[active->second], event.ts_us);
                    heap.active.erase(active);
                } else {
                    // The free supplies neither allocation size nor allocation origin.
                    warning(profile, "Native frees without recorded allocations have unknown sizes and origins.");
                }
                break;
            }
            case NativeHeapEventKind::Reallocate: {
                // A null result does not release the original allocation.
                if (event.address == 0) break;
                auto old = heap.active.find(event.old_address);
                auto current = heap.active.find(event.address);
                if (event.address != event.old_address) {
                    // Windows emits Alloc(new), Free(old), then this summary for a
                    // moved realloc. Keep the actual new allocation's timestamp/stack.
                    if (current != heap.active.end() && old == heap.active.end() &&
                        profile.allocations[current->second].size_bytes == event.size_bytes)
                        break;
                    warning(profile,
                            "A moved native realloc lacks its allocation/free pair; its summary was not replayed.");
                    complete_event_stream = false;
                    break;
                }
                if (old != heap.active.end()) {
                    if (profile.allocations[old->second].size_bytes != event.old_size_bytes)
                        warning(profile, "Native realloc sizes disagree with recorded allocations.");
                    release(profile.allocations[old->second], event.ts_us);
                    heap.active.erase(old);
                } else {
                    warning(profile, "An in-place native realloc has no recorded original allocation.");
                }
                // A resize begins a generation with a new size and recorded origin.
                add_allocation(event.address, event.size_bytes);
                break;
            }
            case NativeHeapEventKind::Destroy:
                profile.heaps[heap.index].end_ts = event.ts_us;
                for (const auto& entry : heap.active) release(profile.allocations[entry.second], event.ts_us);
                heaps.erase(heap_it);
                break;
        }
    }

    for (auto& process_entry : process_heaps) {
        const auto& process = *processes.at(process_entry.first);
        for (auto& heap_entry : process_entry.second) {
            auto& heap = heap_entry.second;
            if (process.end_ts && (!profile.capture_end_ts || *process.end_ts <= *profile.capture_end_ts)) {
                profile.heaps[heap.index].end_ts = *process.end_ts;
                for (const auto& active : heap.active) release(profile.allocations[active.second], *process.end_ts);
            } else if (complete_event_stream && profile.capture_end_ts) {
                for (const auto& active : heap.active)
                    profile.allocations[active.second].end_state = AllocationEnd::LiveAtCaptureEnd;
            }
        }
    }
    if (missing_stack) {
        profile.quality.unresolved_symbols = true;
        profile.quality.warnings.push_back("Some native allocations have no recorded allocation stack.");
    }
    if (profile.allocations.size() == first_allocation)
        warning(profile, "Native heap records contain no allocation instances.");
}
