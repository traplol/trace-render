#include <gtest/gtest.h>
#include "ui/flamegraph_panel.h"

// main [0,100) -> foo [10,50), bar [60,90)
static TraceModel make_model() {
    TraceModel m;
    uint32_t nm = m.intern_string("main"), nf = m.intern_string("foo");
    uint32_t nb = m.intern_string("bar"), cat = m.intern_string("test");

    auto& proc = m.get_or_create_process(1);
    auto& t = proc.get_or_create_thread(1);

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
        uint32_t idx = (uint32_t)m.events_.size();
        m.events_.push_back(e);
        t.event_indices.push_back(idx);
    };

    push(nm, cat, 0, 100, 0, -1, 30);  // 0: main
    push(nf, cat, 10, 40, 1, 0, 40);   // 1: foo
    push(nb, cat, 60, 30, 1, 0, 30);   // 2: bar
    m.min_ts_ = 0;
    m.max_ts_ = 100;
    return m;
}

static const FlameGraphPanel::FlameNode& find(const FlameGraphPanel& p, const TraceModel& m, const std::string& name,
                                              size_t parent) {
    for (size_t ci : p.nodes()[parent].children)
        if (m.get_string(p.nodes()[ci].name_idx) == name) return p.nodes()[ci];
    ADD_FAILURE() << "node " << name << " not found";
    static FlameGraphPanel::FlameNode dummy;
    return dummy;
}

TEST(FlameGraph, TreeStructure) {
    auto m = make_model();
    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.nodes()[p.root()].children.size(), 1u);
    size_t main_n = p.nodes()[p.root()].children[0];
    EXPECT_EQ(m.get_string(p.nodes()[main_n].name_idx), "main");
    EXPECT_EQ(p.nodes()[main_n].children.size(), 2u);
}

TEST(FlameGraph, TimeAggregation) {
    auto m = make_model();
    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    size_t mn = p.nodes()[p.root()].children[0];
    EXPECT_DOUBLE_EQ(p.nodes()[mn].total_time, 100.0);
    EXPECT_DOUBLE_EQ(p.nodes()[mn].self_time, 30.0);

    auto& foo = find(p, m, "foo", mn);
    EXPECT_DOUBLE_EQ(foo.total_time, 40.0);
    EXPECT_DOUBLE_EQ(foo.self_time, 40.0);
    EXPECT_EQ(foo.call_count, 1u);

    auto& bar = find(p, m, "bar", mn);
    EXPECT_DOUBLE_EQ(bar.total_time, 30.0);
    EXPECT_EQ(bar.call_count, 1u);
}

TEST(FlameGraph, CrossThreadMerging) {
    TraceModel m;
    uint32_t na = m.intern_string("A"), cat = m.intern_string("c");
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
        uint32_t idx = (uint32_t)m.events_.size();
        m.events_.push_back(e);
        proc.find_thread(tid)->event_indices.push_back(idx);
    };
    push(1, 50);
    push(2, 30);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.nodes()[p.root()].children.size(), 1u);
    auto& a = p.nodes()[p.nodes()[p.root()].children[0]];
    EXPECT_DOUBLE_EQ(a.total_time, 80.0);
    EXPECT_EQ(a.call_count, 2u);
}

TEST(FlameGraph, ZeroDurationSkipped) {
    TraceModel m;
    uint32_t n = m.intern_string("z"), c = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);
    TraceEvent e;
    e.name_idx = n;
    e.cat_idx = c;
    e.ph = Phase::Complete;
    e.dur = 0;
    e.pid = 1;
    e.tid = 1;
    e.parent_idx = -1;
    m.events_.push_back(e);
    t.event_indices.push_back(0);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.nodes()[p.root()].children.empty());
}

TEST(FlameGraph, RangeExclusion) {
    auto m = make_model();
    ViewState v;
    v.has_range_selection = true;
    v.range_start_ts = 0;
    v.range_end_ts = 55;  // excludes bar [60,90)

    FlameGraphPanel p;
    p.rebuild(m, v);
    size_t mn = p.nodes()[p.root()].children[0];
    ASSERT_EQ(p.nodes()[mn].children.size(), 1u);
    EXPECT_EQ(m.get_string(p.nodes()[p.nodes()[mn].children[0]].name_idx), "foo");
}

TEST(FlameGraph, RangeClampingPartialOverlap) {
    auto m = make_model();
    ViewState v;
    v.has_range_selection = true;
    v.range_start_ts = 30;
    v.range_end_ts = 80;

    FlameGraphPanel p;
    p.rebuild(m, v);
    size_t mn = p.nodes()[p.root()].children[0];

    // main clamped to [30,80) = 50
    EXPECT_DOUBLE_EQ(p.nodes()[mn].total_time, 50.0);
    // foo [10,50) clamped to [30,50) = 20, bar [60,90) clamped to [60,80) = 20
    auto& foo = find(p, m, "foo", mn);
    auto& bar = find(p, m, "bar", mn);
    EXPECT_DOUBLE_EQ(foo.total_time, 20.0);
    EXPECT_DOUBLE_EQ(bar.total_time, 20.0);
    // self = 50 - 40 = 10
    EXPECT_DOUBLE_EQ(p.nodes()[mn].self_time, 10.0);
}

TEST(FlameGraph, HiddenProcessFiltered) {
    auto m = make_model();
    ViewState v;
    v.hidden_pids.insert(1);

    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.nodes()[p.root()].children.empty());
}

TEST(FlameGraph, HiddenCategoryFiltered) {
    auto m = make_model();
    ViewState v;
    v.hidden_cats.insert(m.string_map_["test"]);

    FlameGraphPanel p;
    p.rebuild(m, v);
    EXPECT_TRUE(p.nodes()[p.root()].children.empty());
}

TEST(FlameGraph, DifferentCategoriesSeparated) {
    TraceModel m;
    uint32_t nf = m.intern_string("foo");
    uint32_t c1 = m.intern_string("render"), c2 = m.intern_string("net");
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
        uint32_t idx = (uint32_t)m.events_.size();
        m.events_.push_back(e);
        t.event_indices.push_back(idx);
    };
    push(c1, 0, 50);
    push(c2, 60, 30);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    auto& nodes = p.nodes();
    ASSERT_EQ(nodes[p.root()].children.size(), 2u);
    EXPECT_NE(nodes[nodes[p.root()].children[0]].cat_idx, nodes[nodes[p.root()].children[1]].cat_idx);
}

TEST(FlameGraph, SelfTimeInteriorNode) {
    TraceModel m;
    uint32_t na = m.intern_string("A"), nb = m.intern_string("B"), cat = m.intern_string("c");
    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    TraceEvent ea;
    ea.name_idx = na;
    ea.cat_idx = cat;
    ea.ph = Phase::Complete;
    ea.ts = 0;
    ea.dur = 100;
    ea.pid = 1;
    ea.tid = 1;
    ea.parent_idx = -1;
    ea.self_time = 70;
    m.events_.push_back(ea);
    t.event_indices.push_back(0);

    TraceEvent eb;
    eb.name_idx = nb;
    eb.cat_idx = cat;
    eb.ph = Phase::Complete;
    eb.ts = 20;
    eb.dur = 30;
    eb.pid = 1;
    eb.tid = 1;
    eb.depth = 1;
    eb.parent_idx = 0;
    eb.self_time = 30;
    m.events_.push_back(eb);
    t.event_indices.push_back(1);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    size_t an = p.nodes()[p.root()].children[0];
    EXPECT_DOUBLE_EQ(p.nodes()[an].total_time, 100.0);
    EXPECT_DOUBLE_EQ(p.nodes()[an].self_time, 70.0);
    size_t bn = p.nodes()[an].children[0];
    EXPECT_DOUBLE_EQ(p.nodes()[bn].total_time, 30.0);
    EXPECT_DOUBLE_EQ(p.nodes()[bn].self_time, 30.0);
}

TEST(FlameGraph, DurationBeginEvents) {
    TraceModel m;
    uint32_t nw = m.intern_string("work"), cat = m.intern_string("c");
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
    m.events_.push_back(eb);
    t.event_indices.push_back(0);

    TraceEvent ee;
    ee.name_idx = nw;
    ee.cat_idx = cat;
    ee.ph = Phase::DurationEnd;
    ee.ts = 50;
    ee.pid = 1;
    ee.tid = 1;
    ee.parent_idx = -1;
    ee.is_end_event = true;
    m.events_.push_back(ee);
    t.event_indices.push_back(1);

    ViewState v;
    FlameGraphPanel p;
    p.rebuild(m, v);

    ASSERT_EQ(p.nodes()[p.root()].children.size(), 1u);
    auto& w = p.nodes()[p.nodes()[p.root()].children[0]];
    EXPECT_DOUBLE_EQ(w.total_time, 40.0);
    EXPECT_EQ(w.call_count, 1u);
}
