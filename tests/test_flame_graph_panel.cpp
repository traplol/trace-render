#include <gtest/gtest.h>
#include "ui/flame_graph_panel.h"

// Helpers to navigate the flat sibling-linked tree.
static const FlameNode* find_root(const FlameTree& tree, const TraceModel& m, const std::string& name) {
    for (uint32_t r = tree.first_root; r != UINT32_MAX; r = tree.node(r).next_sibling) {
        if (m.get_string(tree.node(r).name_idx) == name) return &tree.node(r);
    }
    return nullptr;
}

static const FlameNode* find_child(const FlameTree& tree, const FlameNode& parent, const TraceModel& m,
                                   const std::string& name) {
    for (uint32_t c = parent.first_child; c != UINT32_MAX; c = tree.node(c).next_sibling) {
        if (m.get_string(tree.node(c).name_idx) == name) return &tree.node(c);
    }
    return nullptr;
}

static uint32_t count_roots(const FlameTree& tree) {
    uint32_t n = 0;
    for (uint32_t r = tree.first_root; r != UINT32_MAX; r = tree.node(r).next_sibling) n++;
    return n;
}

static uint32_t count_children(const FlameTree& tree, const FlameNode& parent) {
    uint32_t n = 0;
    for (uint32_t c = parent.first_child; c != UINT32_MAX; c = tree.node(c).next_sibling) n++;
    return n;
}

// Factory: main [0,100) -> foo [10,50), bar [60,90)
static TraceModel make_model() {
    TraceModel m;
    uint32_t nm = m.intern_string("main");
    uint32_t nf = m.intern_string("foo");
    uint32_t nb = m.intern_string("bar");
    uint32_t cat = m.intern_string("test");

    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t name, uint32_t c, double ts, double dur, uint8_t depth, int32_t parent, double self) {
        TraceEvent e;
        e.name_idx = name;
        e.cat_idx = c;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.depth = depth;
        e.parent_idx = parent;
        e.self_time = self;
        t.event_indices.push_back(m.add_event(e));
    };

    push(nm, cat, 0, 100, 0, -1, 30);  // 0: main
    push(nf, cat, 10, 40, 1, 0, 40);   // 1: foo
    push(nb, cat, 60, 30, 1, 0, 30);   // 2: bar
    return m;
}

TEST(FlameGraph, SingleEvent) {
    TraceModel m;
    uint32_t n = m.intern_string("work");
    uint32_t c = m.intern_string("cat");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent e;
    e.name_idx = n;
    e.cat_idx = c;
    e.ph = Phase::Complete;
    e.ts = 0;
    e.dur = 50;
    e.pid = 1;
    e.tid = 1;
    e.parent_idx = -1;
    e.self_time = 50;
    t.event_indices.push_back(m.add_event(e));

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];
    ASSERT_EQ(count_roots(tree), 1u);
    const auto* root = find_root(tree, m, "work");
    ASSERT_NE(root, nullptr);
    EXPECT_DOUBLE_EQ(root->total_time, 50.0);
    EXPECT_DOUBLE_EQ(root->self_time, 50.0);
    EXPECT_EQ(root->call_count, 1u);
}

TEST(FlameGraph, TreeStructure) {
    auto m = make_model();
    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];
    ASSERT_EQ(count_roots(tree), 1u);

    const auto* main_node = find_root(tree, m, "main");
    ASSERT_NE(main_node, nullptr);
    EXPECT_EQ(count_children(tree, *main_node), 2u);
}

TEST(FlameGraph, TimeAggregation) {
    auto m = make_model();
    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* mn = find_root(tree, m, "main");
    ASSERT_NE(mn, nullptr);
    EXPECT_DOUBLE_EQ(mn->total_time, 100.0);
    EXPECT_DOUBLE_EQ(mn->self_time, 30.0);

    const auto* foo = find_child(tree, *mn, m, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_DOUBLE_EQ(foo->total_time, 40.0);
    EXPECT_DOUBLE_EQ(foo->self_time, 40.0);
    EXPECT_EQ(foo->call_count, 1u);

    const auto* bar = find_child(tree, *mn, m, "bar");
    ASSERT_NE(bar, nullptr);
    EXPECT_DOUBLE_EQ(bar->total_time, 30.0);
    EXPECT_EQ(bar->call_count, 1u);
}

TEST(FlameGraph, MultipleCalls) {
    TraceModel m;
    uint32_t nf = m.intern_string("func");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    for (int i = 0; i < 3; i++) {
        TraceEvent e;
        e.name_idx = nf;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = i * 100.0;
        e.dur = 50.0;
        e.pid = 1;
        e.tid = 1;
        e.parent_idx = -1;
        e.self_time = 50.0;
        t.event_indices.push_back(m.add_event(e));
    }

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];
    ASSERT_EQ(count_roots(tree), 1u);
    const auto* root = find_root(tree, m, "func");
    ASSERT_NE(root, nullptr);
    EXPECT_DOUBLE_EQ(root->total_time, 150.0);
    EXPECT_EQ(root->call_count, 3u);
}

TEST(FlameGraph, RangeExclusion) {
    auto m = make_model();
    ViewState v;
    v.set_range_selection(0, 55);

    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* mn = find_root(tree, m, "main");
    ASSERT_NE(mn, nullptr);
    EXPECT_EQ(count_children(tree, *mn), 1u);
    EXPECT_NE(find_child(tree, *mn, m, "foo"), nullptr);
}

TEST(FlameGraph, RangeClamping) {
    auto m = make_model();
    ViewState v;
    v.set_range_selection(30, 80);

    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* mn = find_root(tree, m, "main");
    ASSERT_NE(mn, nullptr);
    EXPECT_DOUBLE_EQ(mn->total_time, 50.0);

    const auto* foo = find_child(tree, *mn, m, "foo");
    ASSERT_NE(foo, nullptr);
    EXPECT_DOUBLE_EQ(foo->total_time, 20.0);

    const auto* bar = find_child(tree, *mn, m, "bar");
    ASSERT_NE(bar, nullptr);
    EXPECT_DOUBLE_EQ(bar->total_time, 20.0);

    EXPECT_DOUBLE_EQ(mn->self_time, 10.0);
}

TEST(FlameGraph, HiddenProcessFiltered) {
    auto m = make_model();
    ViewState v;
    v.hide_pid(1);
    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.trees().empty());
}

TEST(FlameGraph, HiddenThreadFiltered) {
    auto m = make_model();
    ViewState v;
    v.hide_tid(1);
    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.trees().empty());
}

TEST(FlameGraph, HiddenCategoryFiltered) {
    auto m = make_model();
    ViewState v;
    v.hide_cat(m.string_map().at("test"));
    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.trees().empty());
}

TEST(FlameGraph, MultipleThreadsSeparateTrees) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t cat = m.intern_string("c");
    auto& proc = m.get_or_create_process(1);
    proc.get_or_create_thread(1);
    proc.get_or_create_thread(2);

    auto push = [&](uint32_t tid, double dur) {
        TraceEvent e;
        e.name_idx = na;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = 0;
        e.dur = dur;
        e.pid = 1;
        e.tid = tid;
        e.parent_idx = -1;
        e.self_time = dur;
        proc.find_thread(tid)->event_indices.push_back(m.add_event(e));
    };
    push(1, 50);
    push(2, 30);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 2u);
    // Sorted by total time descending.
    EXPECT_DOUBLE_EQ(p.trees()[0].root_total_time, 50.0);
    EXPECT_DOUBLE_EQ(p.trees()[1].root_total_time, 30.0);
}

TEST(FlameGraph, ZeroDurationSkipped) {
    TraceModel m;
    uint32_t n = m.intern_string("z");
    uint32_t c = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent e;
    e.name_idx = n;
    e.cat_idx = c;
    e.ph = Phase::Complete;
    e.dur = 0;
    e.pid = 1;
    e.tid = 1;
    e.parent_idx = -1;
    t.event_indices.push_back(m.add_event(e));

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.trees().empty());
}

TEST(FlameGraph, SelfTimeInteriorNode) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t nb = m.intern_string("B");
    uint32_t nc = m.intern_string("C");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t name, double ts, double dur, uint8_t depth, int32_t parent) {
        TraceEvent e;
        e.name_idx = name;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.depth = depth;
        e.parent_idx = parent;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };

    push(na, 0, 100, 0, -1);
    push(nb, 10, 40, 1, 0);
    push(nc, 60, 30, 1, 0);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* a = find_root(tree, m, "A");
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->total_time, 100.0);
    EXPECT_DOUBLE_EQ(a->self_time, 30.0);

    const auto* b = find_child(tree, *a, m, "B");
    ASSERT_NE(b, nullptr);
    EXPECT_DOUBLE_EQ(b->total_time, 40.0);
    EXPECT_DOUBLE_EQ(b->self_time, 40.0);
}

TEST(FlameGraph, DifferentCategoriesSeparated) {
    TraceModel m;
    uint32_t nf = m.intern_string("foo");
    uint32_t c1 = m.intern_string("render");
    uint32_t c2 = m.intern_string("net");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t cat, double ts, double dur) {
        TraceEvent e;
        e.name_idx = nf;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.parent_idx = -1;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };
    push(c1, 0, 50);
    push(c2, 60, 30);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    EXPECT_EQ(count_roots(tree), 2u);
}

TEST(FlameGraph, MixedLeafAndParent) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t nb = m.intern_string("B");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t name, double ts, double dur, uint8_t depth, int32_t parent) {
        TraceEvent e;
        e.name_idx = name;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.depth = depth;
        e.parent_idx = parent;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };

    push(na, 0, 100, 0, -1);
    push(nb, 10, 40, 1, 0);
    push(na, 200, 50, 0, -1);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* a = find_root(tree, m, "A");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->call_count, 2u);
    EXPECT_DOUBLE_EQ(a->total_time, 150.0);
    EXPECT_DOUBLE_EQ(a->self_time, 110.0);
    EXPECT_EQ(count_children(tree, *a), 1u);
}

TEST(FlameGraph, DeeplyNested) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t nb = m.intern_string("B");
    uint32_t nc = m.intern_string("C");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t name, double ts, double dur, uint8_t depth, int32_t parent) {
        TraceEvent e;
        e.name_idx = name;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.depth = depth;
        e.parent_idx = parent;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };

    push(na, 0, 100, 0, -1);
    push(nb, 10, 80, 1, 0);
    push(nc, 20, 50, 2, 1);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    const auto& tree = p.trees()[0];
    const auto* a = find_root(tree, m, "A");
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->total_time, 100.0);
    EXPECT_DOUBLE_EQ(a->self_time, 20.0);

    const auto* b = find_child(tree, *a, m, "B");
    ASSERT_NE(b, nullptr);
    EXPECT_DOUBLE_EQ(b->total_time, 80.0);
    EXPECT_DOUBLE_EQ(b->self_time, 30.0);

    const auto* c = find_child(tree, *b, m, "C");
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->total_time, 50.0);
    EXPECT_DOUBLE_EQ(c->self_time, 50.0);
}

TEST(FlameGraph, DurationBeginEvents) {
    TraceModel m;
    uint32_t nw = m.intern_string("work");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent eb;
    eb.name_idx = nw;
    eb.cat_idx = cat;
    eb.ph = Phase::DurationBegin;
    eb.ts = 10;
    eb.dur = 40;
    eb.pid = 1;
    eb.tid = 1;
    eb.parent_idx = -1;
    eb.self_time = 40;
    t.event_indices.push_back(m.add_event(eb));

    TraceEvent ee;
    ee.name_idx = nw;
    ee.cat_idx = cat;
    ee.ph = Phase::DurationEnd;
    ee.ts = 50;
    ee.pid = 1;
    ee.tid = 1;
    ee.parent_idx = -1;
    ee.is_end_event = true;
    t.event_indices.push_back(m.add_event(ee));

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];
    const auto* root = find_root(tree, m, "work");
    ASSERT_NE(root, nullptr);
    EXPECT_DOUBLE_EQ(root->total_time, 40.0);
    EXPECT_EQ(root->call_count, 1u);
}

TEST(FlameGraph, HiddenParentCategoryPreservesChild) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t nb = m.intern_string("B");
    uint32_t cat_hidden = m.intern_string("hidden");
    uint32_t cat_visible = m.intern_string("visible");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent ea;
    ea.name_idx = na;
    ea.cat_idx = cat_hidden;
    ea.ph = Phase::Complete;
    ea.ts = 0;
    ea.dur = 100;
    ea.pid = 1;
    ea.tid = 1;
    ea.depth = 0;
    ea.parent_idx = -1;
    ea.self_time = 60;
    t.event_indices.push_back(m.add_event(ea));

    TraceEvent eb;
    eb.name_idx = nb;
    eb.cat_idx = cat_visible;
    eb.ph = Phase::Complete;
    eb.ts = 10;
    eb.dur = 40;
    eb.pid = 1;
    eb.tid = 1;
    eb.depth = 1;
    eb.parent_idx = 0;
    eb.self_time = 40;
    t.event_indices.push_back(m.add_event(eb));

    ViewState v;
    v.hide_cat(cat_hidden);

    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];
    ASSERT_EQ(count_roots(tree), 1u);
    const auto* root = find_root(tree, m, "B");
    ASSERT_NE(root, nullptr);
    EXPECT_DOUBLE_EQ(root->total_time, 40.0);
}

// ---------------------------------------------------------------------------
// find_longest_instance tests
// ---------------------------------------------------------------------------

TEST(FlameGraph, FindLongestInstanceSelectsMaxDuration) {
    TraceModel m;
    uint32_t nf = m.intern_string("func");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](double ts, double dur) {
        TraceEvent e;
        e.name_idx = nf;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.parent_idx = -1;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };

    push(0, 10);    // idx 0: short
    push(100, 50);  // idx 1: longest
    push(200, 30);  // idx 2: medium

    int32_t best = FlameGraphPanel::find_longest_instance(m, 1, 1, nf);
    EXPECT_EQ(best, 1);  // should pick the 50us instance, not the first
}

TEST(FlameGraph, FindLongestInstanceScopedToThread) {
    TraceModel m;
    uint32_t nf = m.intern_string("func");
    uint32_t cat = m.intern_string("c");
    auto& proc = m.get_or_create_process(1);
    // Create both threads before taking references to avoid invalidation.
    proc.get_or_create_thread(1);
    proc.get_or_create_thread(2);

    // Thread 1: short instance
    {
        TraceEvent e;
        e.name_idx = nf;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = 0;
        e.dur = 10;
        e.pid = 1;
        e.tid = 1;
        e.parent_idx = -1;
        e.self_time = 10;
        proc.find_thread(1)->event_indices.push_back(m.add_event(e));
    }

    // Thread 2: long instance
    {
        TraceEvent e;
        e.name_idx = nf;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = 0;
        e.dur = 100;
        e.pid = 1;
        e.tid = 2;
        e.parent_idx = -1;
        e.self_time = 100;
        proc.find_thread(2)->event_indices.push_back(m.add_event(e));
    }

    // Searching thread 1 should find the short one (idx 0), not the long one from thread 2
    int32_t best_t1 = FlameGraphPanel::find_longest_instance(m, 1, 1, nf);
    EXPECT_EQ(best_t1, 0);

    // Searching thread 2 should find the long one (idx 1)
    int32_t best_t2 = FlameGraphPanel::find_longest_instance(m, 1, 2, nf);
    EXPECT_EQ(best_t2, 1);
}

TEST(FlameGraph, FindLongestInstanceNoMatch) {
    TraceModel m;
    uint32_t nf = m.intern_string("func");
    uint32_t nother = m.intern_string("other");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent e;
    e.name_idx = nother;
    e.cat_idx = cat;
    e.ph = Phase::Complete;
    e.ts = 0;
    e.dur = 50;
    e.pid = 1;
    e.tid = 1;
    e.parent_idx = -1;
    e.self_time = 50;
    t.event_indices.push_back(m.add_event(e));

    // No event with name "func" in this thread
    int32_t best = FlameGraphPanel::find_longest_instance(m, 1, 1, nf);
    EXPECT_EQ(best, -1);
}

TEST(FlameGraph, FindLongestInstanceInvalidThread) {
    TraceModel m;
    uint32_t nf = m.intern_string("func");
    m.get_or_create_process(1).get_or_create_thread(1);

    // Non-existent thread
    int32_t best = FlameGraphPanel::find_longest_instance(m, 1, 99, nf);
    EXPECT_EQ(best, -1);
}

// Regression: context-parent chain where no intermediate node is a direct leaf.
// Only the deepest node (C) has call_count > 0. A and B are pure context parents
// whose total_time must propagate bottom-up correctly.
TEST(FlameGraph, ContextParentChain) {
    TraceModel m;
    uint32_t na = m.intern_string("A");
    uint32_t nb = m.intern_string("B");
    uint32_t nc = m.intern_string("C");
    uint32_t cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    // Only C is a real event; A and B appear only as context parents via parent_idx.
    TraceEvent ec;
    ec.name_idx = nc;
    ec.cat_idx = cat;
    ec.ph = Phase::Complete;
    ec.ts = 20;
    ec.dur = 50;
    ec.pid = 1;
    ec.tid = 1;
    ec.depth = 2;
    ec.parent_idx = -1;  // will set below
    ec.self_time = 50;

    // Create dummy parent events for the parent chain (not iterated as events themselves).
    TraceEvent ea;
    ea.name_idx = na;
    ea.cat_idx = cat;
    ea.ph = Phase::Complete;
    ea.ts = 0;
    ea.dur = 100;
    ea.pid = 1;
    ea.tid = 1;
    ea.depth = 0;
    ea.parent_idx = -1;
    ea.self_time = 30;
    uint32_t a_idx = m.add_event(ea);

    TraceEvent eb;
    eb.name_idx = nb;
    eb.cat_idx = cat;
    eb.ph = Phase::Complete;
    eb.ts = 10;
    eb.dur = 80;
    eb.pid = 1;
    eb.tid = 1;
    eb.depth = 1;
    eb.parent_idx = (int32_t)a_idx;
    eb.self_time = 30;
    uint32_t b_idx = m.add_event(eb);

    ec.parent_idx = (int32_t)b_idx;
    uint32_t c_idx = m.add_event(ec);

    // Only add C to the thread's event list — A and B exist only as context parents.
    t.event_indices.push_back(c_idx);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.trees().size(), 1u);
    const auto& tree = p.trees()[0];

    // A and B are pure context parents (call_count == 0), total propagated from C.
    const auto* a = find_root(tree, m, "A");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->call_count, 0u);
    EXPECT_DOUBLE_EQ(a->total_time, 50.0);
    EXPECT_DOUBLE_EQ(a->self_time, 0.0);

    const auto* b = find_child(tree, *a, m, "B");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->call_count, 0u);
    EXPECT_DOUBLE_EQ(b->total_time, 50.0);
    EXPECT_DOUBLE_EQ(b->self_time, 0.0);

    const auto* c = find_child(tree, *b, m, "C");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->call_count, 1u);
    EXPECT_DOUBLE_EQ(c->total_time, 50.0);
    EXPECT_DOUBLE_EQ(c->self_time, 50.0);
}
