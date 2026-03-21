#include <gtest/gtest.h>
#include "ui/timeline_view.h"

// Helper to build a minimal set of events for hit-test candidate selection.
static std::vector<TraceEvent> make_events(uint32_t visible_cat, uint32_t hidden_cat) {
    std::vector<TraceEvent> events;

    // Event 0: visible category, depth 0, ts=100 dur=200
    TraceEvent e0;
    e0.ts = 100.0;
    e0.dur = 200.0;
    e0.depth = 0;
    e0.cat_idx = visible_cat;
    e0.ph = Phase::Complete;
    e0.is_end_event = false;
    events.push_back(e0);

    // Event 1: hidden category, depth 0, ts=100 dur=50 (shorter — would win without filter)
    TraceEvent e1;
    e1.ts = 100.0;
    e1.dur = 50.0;
    e1.depth = 0;
    e1.cat_idx = hidden_cat;
    e1.ph = Phase::Complete;
    e1.is_end_event = false;
    events.push_back(e1);

    // Event 2: end event (should always be skipped)
    TraceEvent e2;
    e2.ts = 100.0;
    e2.dur = 10.0;
    e2.depth = 0;
    e2.cat_idx = visible_cat;
    e2.ph = Phase::DurationEnd;
    e2.is_end_event = true;
    events.push_back(e2);

    return events;
}

TEST(TimelineHitTest, HiddenCategoryFilteredOut) {
    const uint32_t visible_cat = 1;
    const uint32_t hidden_cat = 2;
    auto events = make_events(visible_cat, hidden_cat);

    std::vector<uint32_t> candidates = {0, 1};
    std::unordered_set<uint32_t> hidden_cats = {hidden_cat};

    // Event 1 has shorter duration but is hidden — should select event 0
    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/150.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, 0);
}

TEST(TimelineHitTest, AllCandidatesHiddenReturnsNone) {
    const uint32_t hidden_cat = 2;
    auto events = make_events(/*visible_cat=*/1, hidden_cat);

    // Only event 1 (hidden category) as candidate
    std::vector<uint32_t> candidates = {1};
    std::unordered_set<uint32_t> hidden_cats = {hidden_cat};

    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/120.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, -1);
}

TEST(TimelineHitTest, NoCategoryFilterSelectsShortest) {
    const uint32_t cat_a = 1;
    const uint32_t cat_b = 2;
    auto events = make_events(cat_a, cat_b);

    std::vector<uint32_t> candidates = {0, 1};
    std::unordered_set<uint32_t> hidden_cats;  // empty — no filtering

    // Event 1 (dur=50) is shorter than event 0 (dur=200) — should win
    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/120.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, 1);
}

TEST(TimelineHitTest, EndEventsSkipped) {
    const uint32_t cat = 1;
    auto events = make_events(cat, /*hidden_cat=*/99);

    // Only the end event (index 2) as candidate
    std::vector<uint32_t> candidates = {2};
    std::unordered_set<uint32_t> hidden_cats;

    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/105.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, -1);
}

TEST(TimelineHitTest, WrongDepthSkipped) {
    const uint32_t cat = 1;
    auto events = make_events(cat, /*hidden_cat=*/99);

    std::vector<uint32_t> candidates = {0};
    std::unordered_set<uint32_t> hidden_cats;

    // Event 0 is at depth 0, but we click depth 1
    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/1,
                                                         /*click_time=*/150.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, -1);
}

TEST(TimelineHitTest, OutOfTimeRangeSkipped) {
    const uint32_t cat = 1;
    auto events = make_events(cat, /*hidden_cat=*/99);

    std::vector<uint32_t> candidates = {0};
    std::unordered_set<uint32_t> hidden_cats;

    // Event 0 spans [100, 300]. Click at 400 with 5px tolerance — out of range.
    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/400.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, -1);
}

TEST(TimelineHitTest, EmptyCandidatesReturnsNone) {
    std::vector<TraceEvent> events;
    std::vector<uint32_t> candidates;
    std::unordered_set<uint32_t> hidden_cats;

    int32_t result = TimelineView::select_best_candidate(candidates, events, hidden_cats, /*clicked_depth=*/0,
                                                         /*click_time=*/100.0, /*tolerance=*/5.0);
    EXPECT_EQ(result, -1);
}
