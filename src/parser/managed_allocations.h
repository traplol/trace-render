#pragma once
#include "etl_reader.h"
#include "model/profile_data.h"

enum class ManagedAllocationEventKind { Allocate, Collected, GcStart, GcEnd, Runtime, Gap };

// The importer supplies normalized process lifetimes, timestamps and resolved stacks.
// Object IDs belong to the profiler and are never heap addresses.
struct ManagedAllocationEvent {
    ManagedAllocationEventKind kind = ManagedAllocationEventKind::Gap;
    uint64_t raw_qpc = 0, object_id = 0, size_bytes = 0;
    uint32_t pid = 0, gc_number = 0, gc_depth = 0, gc_type = 0;
    uint16_t clr_instance = 0, runtime_sku = 0, runtime_major = 0;
    double ts_us = 0;
    std::string process_id, stack_frame_id, type_id;
    std::vector<uint64_t> addresses, collected_ids;
};

// Returns false for unrelated providers/events. Unsupported or malformed relevant
// records become gaps, so other capabilities can still open.
bool decode_managed_allocation(const EtlRecord& record, ManagedAllocationEvent& event);
void build_managed_allocations(std::vector<ManagedAllocationEvent>& events, ProfileData& profile,
                               bool complete_event_stream, const ImportProgress& progress = {});
