#include <gtest/gtest.h>
#include "model/trace_model.h"
#include "ui/event_browser.h"

// Replicates the sibling-finding logic from DetailPanel::rebuild_siblings()
// to unit-test it without requiring an ImGui context.
static std::vector<uint32_t> find_siblings(const TraceModel& model, uint32_t event_idx) {
    std::vector<uint32_t> result;
    const auto& ev = model.events()[event_idx];
    if (ev.parent_idx < 0) return result;

    const auto* thread = model.find_thread(ev.pid, ev.tid);
    if (!thread) return result;

    const auto& parent = model.events()[ev.parent_idx];
    for (uint32_t idx : thread->event_indices) {
        const auto& sib = model.events()[idx];
        if (sib.ts >= parent.end_ts()) break;
        if (sib.depth != ev.depth) continue;
        if (sib.parent_idx != ev.parent_idx) continue;
        if (sib.dur <= 0) continue;
        if (idx == event_idx) continue;
        result.push_back(idx);
    }
    return result;
}

static TraceModel make_sibling_model() {
    //  0: root [0, 200)  depth=0
    //  1:   A  [0,  50)  depth=1  parent=0
    //  2:   B  [50,100)  depth=1  parent=0
    //  3:     D [50, 80) depth=2  parent=2  (child of B, no siblings)
    //  4:   C  [100,150) depth=1  parent=0
    //  5:   C  [150,200) depth=1  parent=0
    TraceModel m;
    uint32_t n_root = m.intern_string("root");
    uint32_t n_a = m.intern_string("A");
    uint32_t n_b = m.intern_string("B");
    uint32_t n_c = m.intern_string("C");
    uint32_t n_d = m.intern_string("D");
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
        t.event_indices.push_back(m.add_event(e));
    };

    push(n_root, 0, 200);  // 0
    push(n_a, 0, 50);      // 1
    push(n_b, 50, 50);     // 2
    push(n_d, 50, 30);     // 3: child of B
    push(n_c, 100, 50);    // 4
    push(n_c, 150, 50);    // 5

    m.build_index();
    return m;
}

TEST(DetailSiblings, RootEventHasNoSiblings) {
    auto m = make_sibling_model();
    EXPECT_EQ(m.events()[0].depth, 0);
    EXPECT_EQ(m.events()[0].parent_idx, -1);
    auto sibs = find_siblings(m, 0);
    EXPECT_TRUE(sibs.empty());
}

TEST(DetailSiblings, SiblingsAtDepth1) {
    auto m = make_sibling_model();
    // Event 1 (A) should have siblings B, C, C (events 2, 4, 5)
    auto sibs = find_siblings(m, 1);
    EXPECT_EQ(sibs.size(), 3u);
    EXPECT_EQ(m.get_string(m.events()[sibs[0]].name_idx), "B");
    EXPECT_EQ(m.get_string(m.events()[sibs[1]].name_idx), "C");
    EXPECT_EQ(m.get_string(m.events()[sibs[2]].name_idx), "C");
}

TEST(DetailSiblings, SiblingsExcludesSelf) {
    auto m = make_sibling_model();
    auto sibs = find_siblings(m, 2);
    EXPECT_EQ(sibs.size(), 3u);
    for (uint32_t idx : sibs) {
        EXPECT_NE(idx, 2u);
    }
}

TEST(DetailSiblings, OnlyChildHasNoSiblings) {
    auto m = make_sibling_model();
    // Event 3 (D) is the only child of B
    auto sibs = find_siblings(m, 3);
    EXPECT_TRUE(sibs.empty());
}

TEST(DetailSiblings, SiblingsDontCrossParents) {
    TraceModel m;
    uint32_t n_r = m.intern_string("root");
    uint32_t n_c = m.intern_string("child");
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
        t.event_indices.push_back(m.add_event(e));
    };

    push(n_r, 0, 100);    // 0: root1
    push(n_c, 10, 30);    // 1: child of root1
    push(n_r, 200, 100);  // 2: root2
    push(n_c, 210, 30);   // 3: child of root2

    m.build_index();

    EXPECT_TRUE(find_siblings(m, 1).empty());
    EXPECT_TRUE(find_siblings(m, 3).empty());
}

TEST(DetailSiblings, DuplicateNameSiblingsAllFound) {
    auto m = make_sibling_model();
    // Event 4 (first C) should have siblings A, B, and the other C
    auto sibs = find_siblings(m, 4);
    EXPECT_EQ(sibs.size(), 3u);
}

TEST(EventBrowserUnit, SetEntriesPopulatesCount) {
    TraceModel m;
    uint32_t n = m.intern_string("foo");

    EventBrowser browser;
    std::vector<EventBrowser::Entry> entries;
    entries.push_back({0, n, 10.0, 50.0f});
    entries.push_back({1, n, 20.0, 100.0f});
    browser.set_entries(std::move(entries), 30.0, m);
    EXPECT_EQ(browser.entry_count(), 2u);
}

TEST(EventBrowserUnit, ResetClearsEntries) {
    TraceModel m;
    uint32_t n = m.intern_string("bar");

    EventBrowser browser;
    browser.set_entries({{0, n, 5.0, 25.0f}}, 20.0, m);
    EXPECT_EQ(browser.entry_count(), 1u);
    browser.reset();
    EXPECT_EQ(browser.entry_count(), 0u);
}
