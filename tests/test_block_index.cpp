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
