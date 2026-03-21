#include <gtest/gtest.h>
#include "ui/search_panel.h"

static TraceModel make_search_model() {
    TraceModel m;
    uint32_t n_foo = m.intern_string("foo");
    uint32_t n_bar = m.intern_string("bar");
    uint32_t cat = m.intern_string("test");

    auto& t = m.get_or_create_process(1).get_or_create_thread(1);

    auto push = [&](uint32_t name, double ts, double dur) {
        TraceEvent e;
        e.name_idx = name;
        e.cat_idx = cat;
        e.ph = Phase::Complete;
        e.ts = ts;
        e.dur = dur;
        e.pid = 1;
        e.tid = 1;
        e.depth = 0;
        e.parent_idx = -1;
        e.self_time = dur;
        t.event_indices.push_back(m.add_event(e));
    };

    // 3 "foo" events with durations 10, 20, 30 => count=3, avg=20
    push(n_foo, 0, 10);
    push(n_foo, 100, 20);
    push(n_foo, 200, 30);
    // 1 "bar" event with duration 50 => count=1, avg=50
    push(n_bar, 300, 50);

    m.build_index();
    return m;
}

TEST(SearchPanel, BuildNameStatsCountAndAvg) {
    TraceModel m = make_search_model();
    SearchPanel panel;

    // All 4 events
    std::vector<uint32_t> results = {0, 1, 2, 3};
    panel.build_name_stats(m, results);

    uint32_t n_foo = m.intern_string("foo");
    uint32_t n_bar = m.intern_string("bar");

    const auto& stats = panel.name_stats();
    ASSERT_EQ(stats.size(), 2u);

    auto foo_it = stats.find(n_foo);
    ASSERT_NE(foo_it, stats.end());
    EXPECT_EQ(foo_it->second.count, 3u);
    EXPECT_DOUBLE_EQ(foo_it->second.avg_dur, 20.0);

    auto bar_it = stats.find(n_bar);
    ASSERT_NE(bar_it, stats.end());
    EXPECT_EQ(bar_it->second.count, 1u);
    EXPECT_DOUBLE_EQ(bar_it->second.avg_dur, 50.0);
}

TEST(SearchPanel, BuildNameStatsSubsetOfResults) {
    TraceModel m = make_search_model();
    SearchPanel panel;

    // Only first 2 foo events
    std::vector<uint32_t> results = {0, 1};
    panel.build_name_stats(m, results);

    uint32_t n_foo = m.intern_string("foo");
    const auto& stats = panel.name_stats();
    ASSERT_EQ(stats.size(), 1u);

    auto foo_it = stats.find(n_foo);
    ASSERT_NE(foo_it, stats.end());
    EXPECT_EQ(foo_it->second.count, 2u);
    EXPECT_DOUBLE_EQ(foo_it->second.avg_dur, 15.0);
}

TEST(SearchPanel, BuildNameStatsEmpty) {
    TraceModel m = make_search_model();
    SearchPanel panel;

    std::vector<uint32_t> results = {};
    panel.build_name_stats(m, results);

    EXPECT_TRUE(panel.name_stats().empty());
}
