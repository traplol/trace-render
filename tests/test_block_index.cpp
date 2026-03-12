#include <gtest/gtest.h>
#include "model/block_index.h"

class BlockIndexTest : public ::testing::Test {
protected:
    std::vector<TraceEvent> events;
    std::vector<uint32_t> indices;
    BlockIndex index;

    void add_event(double ts, double dur) {
        TraceEvent ev;
        ev.ph = Phase::Complete;
        ev.ts = ts;
        ev.dur = dur;
        uint32_t idx = (uint32_t)events.size();
        events.push_back(ev);
        indices.push_back(idx);
    }
};

TEST_F(BlockIndexTest, EmptyIndex) {
    index.build(indices, events);
    EXPECT_TRUE(index.blocks.empty());

    std::vector<uint32_t> result;
    index.query(0.0, 1000.0, indices, events, result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BlockIndexTest, SingleEvent) {
    add_event(100.0, 50.0);
    index.build(indices, events);

    ASSERT_EQ(index.blocks.size(), 1u);
    EXPECT_DOUBLE_EQ(index.blocks[0].min_ts, 100.0);
    EXPECT_DOUBLE_EQ(index.blocks[0].max_end_ts, 150.0);

    std::vector<uint32_t> result;
    index.query(0.0, 200.0, indices, events, result);
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(BlockIndexTest, QueryMissesBeforeRange) {
    add_event(100.0, 50.0);
    index.build(indices, events);

    std::vector<uint32_t> result;
    index.query(200.0, 300.0, indices, events, result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BlockIndexTest, QueryMissesAfterRange) {
    add_event(100.0, 50.0);
    index.build(indices, events);

    std::vector<uint32_t> result;
    index.query(0.0, 50.0, indices, events, result);
    EXPECT_TRUE(result.empty());
}

TEST_F(BlockIndexTest, QueryFindsOverlapping) {
    add_event(100.0, 100.0);  // 100-200
    add_event(250.0, 50.0);   // 250-300
    add_event(400.0, 100.0);  // 400-500
    index.build(indices, events);

    std::vector<uint32_t> result;
    index.query(150.0, 260.0, indices, events, result);
    // Should find events 0 (100-200 overlaps 150) and 1 (250-300 overlaps 260)
    EXPECT_EQ(result.size(), 2u);
}

TEST_F(BlockIndexTest, MultipleBlocks) {
    // Add enough events to span multiple blocks (BLOCK_SIZE = 256)
    for (int i = 0; i < 300; i++) {
        add_event(i * 10.0, 5.0);
    }
    index.build(indices, events);

    EXPECT_EQ(index.blocks.size(), 2u);
    EXPECT_EQ(index.blocks[0].count, 256u);
    EXPECT_EQ(index.blocks[1].count, 44u);

    // Query a small range that only hits the second block
    std::vector<uint32_t> result;
    index.query(2900.0, 2950.0, indices, events, result);
    // Events at ts=2900 (dur=5) and ts=2910,2920,...,2950
    EXPECT_FALSE(result.empty());
    for (uint32_t idx : result) {
        EXPECT_GE(events[idx].end_ts(), 2900.0);
        EXPECT_LE(events[idx].ts, 2950.0);
    }
}

TEST_F(BlockIndexTest, LongEventSpanningLaterBlocks) {
    // Regression: a long-duration event in block 0 has max_end_ts that extends
    // past block 1's events. Without monotonic max_end_ts propagation, binary
    // search could skip block 0 when querying the region it spans.
    //
    // Block 0: event 0 at ts=0 dur=5000 (end_ts=5000), then short events ts=10..2550
    // Block 1: short events ts=2560..2990 (all end before 3000)
    // Query [4000, 4500] should find event 0 (its end_ts=5000 overlaps)

    // Event 0: long event spanning way past block 1
    add_event(0.0, 5000.0);

    // Fill rest of block 0 with short events (need 255 more to fill BLOCK_SIZE=256)
    for (int i = 1; i < 256; i++) {
        add_event(i * 10.0, 5.0);
    }

    // Block 1: short events well before the long event's end
    for (int i = 0; i < 50; i++) {
        add_event(2560.0 + i * 10.0, 5.0);
    }

    index.build(indices, events);
    ASSERT_EQ(index.blocks.size(), 2u);

    // Block 0's max_end_ts should be 5000 (from the long event)
    EXPECT_DOUBLE_EQ(index.blocks[0].max_end_ts, 5000.0);

    // Block 1's max_end_ts should be propagated up to at least 5000
    // (monotonic guarantee for binary search correctness)
    EXPECT_GE(index.blocks[1].max_end_ts, index.blocks[0].max_end_ts);

    // Query a range that only the long event covers — must not be missed
    std::vector<uint32_t> result;
    index.query(4000.0, 4500.0, indices, events, result);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 0u);  // The long event at index 0
}
