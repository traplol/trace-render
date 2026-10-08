#pragma once
#include "model/profile_data.h"
#include <cstdint>
#include <string>
#include <vector>

enum class NativeHeapEventKind { Create, Allocate, Reallocate, Free, Destroy };

// Events use one normalized clock and are ordered by time, then original record order.
// The decoder owns process generations and stack correlation; handles are raw heap addresses.
struct NativeHeapEvent {
    NativeHeapEventKind kind = NativeHeapEventKind::Allocate;
    double ts_us = 0;
    uint64_t raw_qpc = 0;
    std::string process_id;
    uint32_t tid = 0;
    uint64_t heap_handle = 0;
    uint64_t address = 0;
    uint64_t size_bytes = 0;
    uint64_t old_address = 0;
    uint64_t old_size_bytes = 0;
    std::string stack_frame_id;
};

// Appends native allocations/heaps once per capture. False coverage records
// internal gaps and leaves unfinished ends unknown. Late starts are reported separately.
void build_native_allocations(const std::vector<NativeHeapEvent>& events, ProfileData& profile,
                              bool complete_event_stream);
