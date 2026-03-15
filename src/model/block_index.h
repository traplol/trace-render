#pragma once
#include "trace_event.h"
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cfloat>
#include <functional>

class BlockIndex {
public:
    static constexpr size_t BLOCK_SIZE = 256;

    struct Block {
        double min_ts;
        double max_end_ts;
        double local_max_end_ts;  // max_end_ts of events in THIS block only (not propagated)
        uint32_t start_idx;
        uint32_t count;
        uint32_t depth_mask;  // bitmask of depths present (bits 0-31)
    };

    const std::vector<Block>& blocks() const { return blocks_; }

    void build(const std::vector<uint32_t>& event_indices, const std::vector<TraceEvent>& events) {
        blocks_.clear();
        if (event_indices.empty()) return;

        for (size_t i = 0; i < event_indices.size(); i += BLOCK_SIZE) {
            Block blk;
            blk.start_idx = (uint32_t)i;
            blk.count = (uint32_t)std::min(BLOCK_SIZE, event_indices.size() - i);
            blk.min_ts = DBL_MAX;
            blk.local_max_end_ts = -DBL_MAX;
            blk.depth_mask = 0;

            for (size_t j = i; j < i + blk.count; j++) {
                const auto& ev = events[event_indices[j]];
                blk.min_ts = std::min(blk.min_ts, ev.ts);
                blk.local_max_end_ts = std::max(blk.local_max_end_ts, ev.end_ts());
                if (ev.depth < 32) {
                    blk.depth_mask |= (1u << ev.depth);
                }
            }
            // Propagate max_end_ts monotonically for binary search correctness
            blk.max_end_ts = blk.local_max_end_ts;
            if (!blocks_.empty()) {
                blk.max_end_ts = std::max(blk.max_end_ts, blocks_.back().max_end_ts);
            }
            blocks_.push_back(blk);
        }
    }

    void query(double start_ts, double end_ts, const std::vector<uint32_t>& event_indices,
               const std::vector<TraceEvent>& events, std::vector<uint32_t>& out) const {
        if (blocks_.empty()) return;

        // Binary search: find first block whose max_end_ts >= start_ts
        size_t lo = 0, hi = blocks_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (blocks_[mid].max_end_ts < start_ts)
                lo = mid + 1;
            else
                hi = mid;
        }

        for (size_t bi = lo; bi < blocks_.size(); bi++) {
            const auto& blk = blocks_[bi];
            if (blk.min_ts > end_ts) break;
            if (blk.max_end_ts < start_ts) continue;

            for (uint32_t j = 0; j < blk.count; j++) {
                uint32_t idx = event_indices[blk.start_idx + j];
                const auto& ev = events[idx];
                if (ev.ts > end_ts) break;
                if (ev.end_ts() >= start_ts) {
                    out.push_back(idx);
                }
            }
        }
    }

    // Find first block index overlapping [start_ts, end_ts] via binary search
    size_t find_first_block(double start_ts) const {
        size_t lo = 0, hi = blocks_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (blocks_[mid].max_end_ts < start_ts)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

private:
    std::vector<Block> blocks_;
};
