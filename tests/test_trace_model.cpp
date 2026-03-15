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
    EXPECT_EQ(model.processes().size(), 1u);
}

TEST_F(TraceModelTest, GetOrCreateProcessMultiple) {
    model.get_or_create_process(1);
    model.get_or_create_process(2);
    model.get_or_create_process(3);
    EXPECT_EQ(model.processes().size(), 3u);
}

TEST_F(TraceModelTest, FindProcessReturnsNullIfMissing) {
    EXPECT_EQ(model.find_process(999), nullptr);
}

TEST_F(TraceModelTest, ClearResetsEverything) {
    model.intern_string("foo");
    model.get_or_create_process(1);
    model.add_event({});
    model.add_args("{}");

    model.clear();

    EXPECT_TRUE(model.events().empty());
    EXPECT_TRUE(model.strings().empty());
    EXPECT_TRUE(model.string_map().empty());
    EXPECT_TRUE(model.args().empty());
    EXPECT_TRUE(model.processes().empty());
    EXPECT_TRUE(model.counter_series().empty());
    EXPECT_TRUE(model.flow_groups().empty());
    EXPECT_DOUBLE_EQ(model.min_ts(), 1e18);
    EXPECT_DOUBLE_EQ(model.max_ts(), -1e18);
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
    model.add_event(b_ev);
    thread.event_indices.push_back(0);

    // Add E event at ts=200
    TraceEvent e_ev;
    e_ev.name_idx = model.intern_string("task");
    e_ev.ph = Phase::DurationEnd;
    e_ev.ts = 200.0;
    e_ev.pid = 1;
    e_ev.tid = 1;
    model.add_event(e_ev);
    thread.event_indices.push_back(1);

    model.build_index();

    // B event should have dur=100, E event should be marked
    EXPECT_DOUBLE_EQ(model.events()[0].dur, 100.0);
    EXPECT_TRUE(model.events()[1].is_end_event);

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
    model.add_event(outer);
    thread.event_indices.push_back(0);

    // Inner event: ts=120, dur=50
    TraceEvent inner;
    inner.ph = Phase::Complete;
    inner.ts = 120.0;
    inner.dur = 50.0;
    inner.pid = 1;
    inner.tid = 1;
    model.add_event(inner);
    thread.event_indices.push_back(1);

    model.build_index();

    EXPECT_EQ(model.events()[0].depth, 0);
    EXPECT_EQ(model.events()[1].depth, 1);
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
    model.add_event(ev1);
    thread.event_indices.push_back(0);

    TraceEvent ev2;
    ev2.ph = Phase::Complete;
    ev2.ts = 1000.0;
    ev2.dur = 200.0;
    ev2.pid = 1;
    ev2.tid = 1;
    model.add_event(ev2);
    thread.event_indices.push_back(1);

    model.build_index();

    EXPECT_DOUBLE_EQ(model.min_ts(), 500.0);
    EXPECT_DOUBLE_EQ(model.max_ts(), 1200.0);
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
    model.add_event(ev1);
    thread.event_indices.push_back(0);

    TraceEvent ev2;
    ev2.ph = Phase::Complete;
    ev2.name_idx = name_idx;
    ev2.ts = 100.0;
    ev2.dur = 80.0;  // longer
    ev2.pid = 1;
    ev2.tid = 1;
    model.add_event(ev2);
    thread.event_indices.push_back(1);

    // Third event with different timestamp (should not be deduped)
    TraceEvent ev3;
    ev3.ph = Phase::Complete;
    ev3.name_idx = name_idx;
    ev3.ts = 300.0;
    ev3.dur = 20.0;
    ev3.pid = 1;
    ev3.tid = 1;
    model.add_event(ev3);
    thread.event_indices.push_back(2);

    model.build_index();

    // Should have 2 events: the longer duplicate and the distinct one
    EXPECT_EQ(thread.event_indices.size(), 2u);
    // First should be the longer-duration duplicate (index 1, dur=80)
    EXPECT_DOUBLE_EQ(model.events()[thread.event_indices[0]].dur, 80.0);
    EXPECT_DOUBLE_EQ(model.events()[thread.event_indices[1]].ts, 300.0);
}

TEST_F(TraceModelTest, BuildIndexSortsProcessesAndThreads) {
    auto& p2 = model.get_or_create_process(2);
    p2.sort_index = 10;
    auto& p1 = model.get_or_create_process(1);
    p1.sort_index = 5;

    model.build_index();

    EXPECT_EQ(model.processes()[0].pid, 1u);
    EXPECT_EQ(model.processes()[1].pid, 2u);
}

TEST_F(TraceModelTest, BuildIndexCounterSeriesMinMax) {
    auto& cs = model.find_or_create_counter_series(1, "Memory");
    cs.points = {{100.0, 50.0}, {200.0, 30.0}, {300.0, 80.0}};

    model.build_index();

    EXPECT_DOUBLE_EQ(model.counter_series()[0].min_val, 30.0);
    EXPECT_DOUBLE_EQ(model.counter_series()[0].max_val, 80.0);
}

// --- Helper to build a nested call stack model ---
// Creates events: root (depth 0) -> mid (depth 1) -> leaf (depth 2)
static TraceModel make_nested_model() {
    TraceModel m;
    m.intern_string("");  // idx 0

    auto& proc = m.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Event 0: root, ts=100, dur=500
    TraceEvent root;
    root.ph = Phase::Complete;
    root.name_idx = m.intern_string("root");
    root.ts = 100.0;
    root.dur = 500.0;
    root.pid = 1;
    root.tid = 1;
    m.add_event(root);
    thread.event_indices.push_back(0);

    // Event 1: mid, ts=150, dur=300
    TraceEvent mid;
    mid.ph = Phase::Complete;
    mid.name_idx = m.intern_string("mid");
    mid.ts = 150.0;
    mid.dur = 300.0;
    mid.pid = 1;
    mid.tid = 1;
    m.add_event(mid);
    thread.event_indices.push_back(1);

    // Event 2: leaf, ts=200, dur=100
    TraceEvent leaf;
    leaf.ph = Phase::Complete;
    leaf.name_idx = m.intern_string("leaf");
    leaf.ts = 200.0;
    leaf.dur = 100.0;
    leaf.pid = 1;
    leaf.tid = 1;
    m.add_event(leaf);
    thread.event_indices.push_back(2);

    m.build_index();
    return m;
}

TEST(CallStack, FindParentEventReturnsParent) {
    auto model = make_nested_model();
    // leaf (idx 2) parent should be mid (idx 1)
    EXPECT_EQ(model.find_parent_event(2), 1);
    // mid (idx 1) parent should be root (idx 0)
    EXPECT_EQ(model.find_parent_event(1), 0);
}

TEST(CallStack, FindParentEventReturnsMinusOneForRoot) {
    auto model = make_nested_model();
    EXPECT_EQ(model.find_parent_event(0), -1);
}

TEST(CallStack, FindParentEventOutOfBounds) {
    auto model = make_nested_model();
    EXPECT_EQ(model.find_parent_event(999), -1);
}

TEST(CallStack, BuildCallStackFromLeaf) {
    auto model = make_nested_model();
    auto stack = model.build_call_stack(2);
    ASSERT_EQ(stack.size(), 3u);
    // Root first, leaf last
    EXPECT_EQ(model.get_string(model.events()[stack[0]].name_idx), "root");
    EXPECT_EQ(model.get_string(model.events()[stack[1]].name_idx), "mid");
    EXPECT_EQ(model.get_string(model.events()[stack[2]].name_idx), "leaf");
}

TEST(CallStack, BuildCallStackFromMid) {
    auto model = make_nested_model();
    auto stack = model.build_call_stack(1);
    ASSERT_EQ(stack.size(), 2u);
    EXPECT_EQ(model.get_string(model.events()[stack[0]].name_idx), "root");
    EXPECT_EQ(model.get_string(model.events()[stack[1]].name_idx), "mid");
}

TEST(CallStack, BuildCallStackFromRoot) {
    auto model = make_nested_model();
    auto stack = model.build_call_stack(0);
    ASSERT_EQ(stack.size(), 1u);
    EXPECT_EQ(model.get_string(model.events()[stack[0]].name_idx), "root");
}

TEST(CallStack, ComputeSelfTimeLeaf) {
    auto model = make_nested_model();
    // leaf (idx 2) has no children, self time == wall time (100)
    EXPECT_DOUBLE_EQ(model.compute_self_time(2), 100.0);
}

TEST(CallStack, ComputeSelfTimeMid) {
    auto model = make_nested_model();
    // mid (idx 1) has dur=300, leaf child dur=100, so self=200
    EXPECT_DOUBLE_EQ(model.compute_self_time(1), 200.0);
}

TEST(CallStack, ComputeSelfTimeRoot) {
    auto model = make_nested_model();
    // root (idx 0) has dur=500, mid child dur=300, so self=200
    EXPECT_DOUBLE_EQ(model.compute_self_time(0), 200.0);
}

TEST(CallStack, ComputeSelfTimeOutOfBounds) {
    auto model = make_nested_model();
    EXPECT_DOUBLE_EQ(model.compute_self_time(999), 0.0);
}

TEST(CallStack, ParentIdxPrecomputedCorrectly) {
    auto model = make_nested_model();
    EXPECT_EQ(model.events()[0].parent_idx, -1);  // root has no parent
    EXPECT_EQ(model.events()[1].parent_idx, 0);   // mid -> root
    EXPECT_EQ(model.events()[2].parent_idx, 1);   // leaf -> mid
}

TEST(CallStack, SelfTimePrecomputedCorrectly) {
    auto model = make_nested_model();
    // root: dur=500, child mid=300, self=200
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 200.0);
    // mid: dur=300, child leaf=100, self=200
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 200.0);
    // leaf: dur=100, no children, self=100
    EXPECT_DOUBLE_EQ(model.events()[2].self_time, 100.0);
}

TEST(CallStack, SelfTimeMultipleChildren) {
    TraceModel m;
    m.intern_string("");
    auto& proc = m.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Parent: ts=0, dur=1000
    TraceEvent parent;
    parent.ph = Phase::Complete;
    parent.name_idx = m.intern_string("parent");
    parent.ts = 0.0;
    parent.dur = 1000.0;
    parent.pid = 1;
    parent.tid = 1;
    m.add_event(parent);
    thread.event_indices.push_back(0);

    // Child A: ts=100, dur=200
    TraceEvent childA;
    childA.ph = Phase::Complete;
    childA.name_idx = m.intern_string("childA");
    childA.ts = 100.0;
    childA.dur = 200.0;
    childA.pid = 1;
    childA.tid = 1;
    m.add_event(childA);
    thread.event_indices.push_back(1);

    // Child B: ts=500, dur=300
    TraceEvent childB;
    childB.ph = Phase::Complete;
    childB.name_idx = m.intern_string("childB");
    childB.ts = 500.0;
    childB.dur = 300.0;
    childB.pid = 1;
    childB.tid = 1;
    m.add_event(childB);
    thread.event_indices.push_back(2);

    m.build_index();

    // parent self = 1000 - 200 - 300 = 500
    EXPECT_DOUBLE_EQ(m.events()[0].self_time, 500.0);
    EXPECT_EQ(m.events()[1].parent_idx, 0);
    EXPECT_EQ(m.events()[2].parent_idx, 0);
}

// --- Same-timestamp parent/child ---

static TraceModel make_same_ts_model() {
    TraceModel m;
    m.intern_string("");  // idx 0

    auto& proc = m.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Parent: ts=100, dur=50
    TraceEvent parent;
    parent.ph = Phase::Complete;
    parent.name_idx = m.intern_string("parent");
    parent.ts = 100.0;
    parent.dur = 50.0;
    parent.pid = 1;
    parent.tid = 1;
    m.add_event(parent);
    thread.event_indices.push_back(0);

    // Child starts at exact same timestamp, shorter duration
    TraceEvent child;
    child.ph = Phase::Complete;
    child.name_idx = m.intern_string("child");
    child.ts = 100.0;
    child.dur = 20.0;
    child.pid = 1;
    child.tid = 1;
    m.add_event(child);
    thread.event_indices.push_back(1);

    m.build_index();
    return m;
}

TEST(SameTimestamp, DepthAssignment) {
    auto model = make_same_ts_model();
    // Parent (longer dur) should be depth 0, child (shorter dur) depth 1
    EXPECT_EQ(model.events()[0].depth, 0);
    EXPECT_EQ(model.events()[1].depth, 1);
}

TEST(SameTimestamp, FindParent) {
    auto model = make_same_ts_model();
    EXPECT_EQ(model.find_parent_event(1), 0);
    EXPECT_EQ(model.find_parent_event(0), -1);
}

TEST(SameTimestamp, CallStack) {
    auto model = make_same_ts_model();
    auto stack = model.build_call_stack(1);
    ASSERT_EQ(stack.size(), 2u);
    EXPECT_EQ(model.get_string(model.events()[stack[0]].name_idx), "parent");
    EXPECT_EQ(model.get_string(model.events()[stack[1]].name_idx), "child");
}

TEST(SameTimestamp, SelfTime) {
    auto model = make_same_ts_model();
    // Parent dur=50, child dur=20, so parent self=30
    EXPECT_DOUBLE_EQ(model.compute_self_time(0), 30.0);
    // Child has no children, self=20
    EXPECT_DOUBLE_EQ(model.compute_self_time(1), 20.0);
}

TEST(SameTimestamp, ThreeLevelsSameTimestamp) {
    TraceModel m;
    m.intern_string("");

    auto& proc = m.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // Grandparent: ts=100, dur=100
    TraceEvent gp;
    gp.ph = Phase::Complete;
    gp.name_idx = m.intern_string("grandparent");
    gp.ts = 100.0;
    gp.dur = 100.0;
    gp.pid = 1;
    gp.tid = 1;
    m.add_event(gp);
    thread.event_indices.push_back(0);

    // Parent: ts=100, dur=60
    TraceEvent parent;
    parent.ph = Phase::Complete;
    parent.name_idx = m.intern_string("parent");
    parent.ts = 100.0;
    parent.dur = 60.0;
    parent.pid = 1;
    parent.tid = 1;
    m.add_event(parent);
    thread.event_indices.push_back(1);

    // Child: ts=100, dur=10
    TraceEvent child;
    child.ph = Phase::Complete;
    child.name_idx = m.intern_string("child");
    child.ts = 100.0;
    child.dur = 10.0;
    child.pid = 1;
    child.tid = 1;
    m.add_event(child);
    thread.event_indices.push_back(2);

    m.build_index();

    EXPECT_EQ(m.events()[0].depth, 0);
    EXPECT_EQ(m.events()[1].depth, 1);
    EXPECT_EQ(m.events()[2].depth, 2);

    auto stack = m.build_call_stack(2);
    ASSERT_EQ(stack.size(), 3u);
    EXPECT_EQ(m.get_string(m.events()[stack[0]].name_idx), "grandparent");
    EXPECT_EQ(m.get_string(m.events()[stack[1]].name_idx), "parent");
    EXPECT_EQ(m.get_string(m.events()[stack[2]].name_idx), "child");
}
