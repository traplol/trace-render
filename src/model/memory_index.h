#pragma once
#include "profile_data.h"
#include "trace_event.h"
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AllocationAmount {
    uint64_t bytes = 0;
    uint64_t count = 0;
};

struct OutstandingMemoryAmount {
    AllocationAmount known;
    AllocationAmount uncertain;
};

struct OutstandingStack {
    std::string process_id;
    int32_t frame_index = -1;  // -1 is an allocation with no valid origin stack
    OutstandingMemoryAmount total;
};

struct OutstandingFunction {
    std::string process_id;
    int32_t frame_index = -1;  // representative frame of the symbol, never a display-name identity
    OutstandingMemoryAmount inclusive;
    OutstandingMemoryAmount exclusive;
};

struct OutstandingMemory {
    OutstandingMemoryAmount total;
    std::vector<OutstandingStack> stacks;
    std::vector<OutstandingFunction> functions;
    uint64_t unknown_birth_count = 0;  // omitted by a birth-range filter
    bool incomplete = false;
    std::string error;  // includes overflow; totals are empty on failure
};

class MemoryIndex {
public:
    void build(const ProfileData& profile, const std::vector<StackFrame>& frames,
               const std::vector<std::string>& strings);

    // Allocation lifetime [allocated_ts, freed_ts). Birth filters use [first, second).
    // Known live-at-end observations apply only through capture_end_ts, inclusive.
    // Managed survival proves liveness through the last positive checkpoint;
    // a negative checkpoint bounds absence without supplying an exact free time.
    OutstandingMemory query(const ProfileData& profile, double ts,
                            std::optional<std::pair<double, double>> born_between = std::nullopt,
                            const std::string& process_id = {}, AllocationKind kind = AllocationKind::Native) const;

    bool stack_contains_function(int32_t leaf_frame, int32_t function_frame) const;

    void clear();

private:
    struct Entry {
        size_t allocation = 0;
        double start = 0;
        double end = 0;
        double survived_through = 0;
        int32_t frame = -1;
    };
    struct Block {
        size_t begin = 0;
        size_t end = 0;
        double max_end = 0;
    };
    std::vector<Entry> entries_;
    std::vector<Block> blocks_;
    std::vector<int32_t> frame_functions_;
    std::vector<std::vector<int32_t>> stack_functions_;
};
