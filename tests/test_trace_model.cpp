#include <gtest/gtest.h>
#include "model/trace_model.h"

class TraceModelTest : public ::testing::Test {
protected:
    TraceModel model;
};

TEST_F(TraceModelTest, InternStringReturnsConsistentIndex) {
    uint32_t idx1 = model.intern_string("hello");
    uint32_t idx2 = model.intern_string("world");
    uint32_t idx3 = model.intern_string("hello");
    EXPECT_EQ(idx1, idx3);
    EXPECT_NE(idx1, idx2);
}

TEST_F(TraceModelTest, GetStringReturnsCorrectValue) {
    uint32_t idx = model.intern_string("test_string");
    EXPECT_EQ(model.get_string(idx), "test_string");
}

TEST_F(TraceModelTest, GetStringOutOfBoundsReturnsEmpty) {
    EXPECT_EQ(model.get_string(9999), "");
}

TEST_F(TraceModelTest, GetOrCreateProcessCreatesOnce) {
    auto& p1 = model.get_or_create_process(42);
    p1.name = "MyProcess";
    auto& p2 = model.get_or_create_process(42);
    EXPECT_EQ(&p1, &p2);
    EXPECT_EQ(p2.name, "MyProcess");
    EXPECT_EQ(model.processes_.size(), 1u);
}

TEST_F(TraceModelTest, GetOrCreateProcessMultiple) {
    model.get_or_create_process(1);
    model.get_or_create_process(2);
    model.get_or_create_process(3);
    EXPECT_EQ(model.processes_.size(), 3u);
}

TEST_F(TraceModelTest, FindProcessReturnsNullIfMissing) {
    EXPECT_EQ(model.find_process(999), nullptr);
}

TEST_F(TraceModelTest, ClearResetsEverything) {
    model.intern_string("foo");
    model.get_or_create_process(1);
    model.events_.push_back({});
    model.args_.push_back("{}");
    model.min_ts_ = 0;
    model.max_ts_ = 100;

    model.clear();

    EXPECT_TRUE(model.events_.empty());
    EXPECT_TRUE(model.strings_.empty());
    EXPECT_TRUE(model.string_map_.empty());
    EXPECT_TRUE(model.args_.empty());
    EXPECT_TRUE(model.processes_.empty());
    EXPECT_TRUE(model.counter_series_.empty());
    EXPECT_TRUE(model.flow_groups_.empty());
    EXPECT_DOUBLE_EQ(model.min_ts_, 1e18);
    EXPECT_DOUBLE_EQ(model.max_ts_, -1e18);
}

TEST_F(TraceModelTest, BuildIndexMatchesBEPairs) {
    auto& proc = model.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Add B event at ts=100
    TraceEvent b_ev;
    b_ev.name_idx = model.intern_string("task");
    b_ev.ph = Phase::DurationBegin;
    b_ev.ts = 100.0;
    b_ev.pid = 1;
    b_ev.tid = 1;
    model.events_.push_back(b_ev);
    thread.event_indices.push_back(0);

    // Add E event at ts=200
    TraceEvent e_ev;
    e_ev.name_idx = model.intern_string("task");
    e_ev.ph = Phase::DurationEnd;
    e_ev.ts = 200.0;
    e_ev.pid = 1;
    e_ev.tid = 1;
    model.events_.push_back(e_ev);
    thread.event_indices.push_back(1);

    model.build_index();

    // B event should have dur=100, E event should be marked
    EXPECT_DOUBLE_EQ(model.events_[0].dur, 100.0);
    EXPECT_TRUE(model.events_[1].is_end_event);

    // Only the B event should remain in thread indices
    EXPECT_EQ(thread.event_indices.size(), 1u);
    EXPECT_EQ(thread.event_indices[0], 0u);
}

TEST_F(TraceModelTest, BuildIndexComputesDepth) {
    auto& proc = model.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Outer event: ts=100, dur=200
    TraceEvent outer;
    outer.ph = Phase::Complete;
    outer.ts = 100.0;
    outer.dur = 200.0;
    outer.pid = 1;
    outer.tid = 1;
    model.events_.push_back(outer);
    thread.event_indices.push_back(0);

    // Inner event: ts=120, dur=50
    TraceEvent inner;
    inner.ph = Phase::Complete;
    inner.ts = 120.0;
    inner.dur = 50.0;
    inner.pid = 1;
    inner.tid = 1;
    model.events_.push_back(inner);
    thread.event_indices.push_back(1);

    model.build_index();

    EXPECT_EQ(model.events_[0].depth, 0);
    EXPECT_EQ(model.events_[1].depth, 1);
    EXPECT_EQ(thread.max_depth, 1);
}

TEST_F(TraceModelTest, BuildIndexComputesTimeRange) {
    auto& proc = model.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    TraceEvent ev1;
    ev1.ph = Phase::Complete;
    ev1.ts = 500.0;
    ev1.dur = 100.0;
    ev1.pid = 1;
    ev1.tid = 1;
    model.events_.push_back(ev1);
    thread.event_indices.push_back(0);

    TraceEvent ev2;
    ev2.ph = Phase::Complete;
    ev2.ts = 1000.0;
    ev2.dur = 200.0;
    ev2.pid = 1;
    ev2.tid = 1;
    model.events_.push_back(ev2);
    thread.event_indices.push_back(1);

    model.build_index();

    EXPECT_DOUBLE_EQ(model.min_ts_, 500.0);
    EXPECT_DOUBLE_EQ(model.max_ts_, 1200.0);
}

TEST_F(TraceModelTest, BuildIndexDeduplicatesSameNameAndTimestamp) {
    auto& proc = model.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    uint32_t name_idx = model.intern_string("task");

    // Two events with same name and timestamp, different durations
    TraceEvent ev1;
    ev1.ph = Phase::Complete;
    ev1.name_idx = name_idx;
    ev1.ts = 100.0;
    ev1.dur = 50.0;
    ev1.pid = 1;
    ev1.tid = 1;
    model.events_.push_back(ev1);
    thread.event_indices.push_back(0);

    TraceEvent ev2;
    ev2.ph = Phase::Complete;
    ev2.name_idx = name_idx;
    ev2.ts = 100.0;
    ev2.dur = 80.0; // longer
    ev2.pid = 1;
    ev2.tid = 1;
    model.events_.push_back(ev2);
    thread.event_indices.push_back(1);

    // Third event with different timestamp (should not be deduped)
    TraceEvent ev3;
    ev3.ph = Phase::Complete;
    ev3.name_idx = name_idx;
    ev3.ts = 300.0;
    ev3.dur = 20.0;
    ev3.pid = 1;
    ev3.tid = 1;
    model.events_.push_back(ev3);
    thread.event_indices.push_back(2);

    model.build_index();

    // Should have 2 events: the longer duplicate and the distinct one
    EXPECT_EQ(thread.event_indices.size(), 2u);
    // First should be the longer-duration duplicate (index 1, dur=80)
    EXPECT_DOUBLE_EQ(model.events_[thread.event_indices[0]].dur, 80.0);
    EXPECT_DOUBLE_EQ(model.events_[thread.event_indices[1]].ts, 300.0);
}

TEST_F(TraceModelTest, BuildIndexSortsProcessesAndThreads) {
    auto& p2 = model.get_or_create_process(2);
    p2.sort_index = 10;
    auto& p1 = model.get_or_create_process(1);
    p1.sort_index = 5;

    model.build_index();

    EXPECT_EQ(model.processes_[0].pid, 1u);
    EXPECT_EQ(model.processes_[1].pid, 2u);
}

TEST_F(TraceModelTest, BuildIndexCounterSeriesMinMax) {
    CounterSeries cs;
    cs.pid = 1;
    cs.name = "Memory";
    cs.points = {{100.0, 50.0}, {200.0, 30.0}, {300.0, 80.0}};
    model.counter_series_.push_back(cs);

    model.build_index();

    EXPECT_DOUBLE_EQ(model.counter_series_[0].min_val, 30.0);
    EXPECT_DOUBLE_EQ(model.counter_series_[0].max_val, 80.0);
}
