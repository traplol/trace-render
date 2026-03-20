#include <gtest/gtest.h>
#include "ui/view_state.h"
#include "ui/range_stats.h"
#include <cmath>

TEST(ViewState, DefaultValues) {
    ViewState vs;
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 0.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 1000.0);
    EXPECT_EQ(vs.selected_event_idx(), -1);
    EXPECT_TRUE(vs.show_flows());
}

TEST(ViewState, TimeToX) {
    ViewState vs;
    vs.set_view_start_ts(0.0);
    vs.set_view_end_ts(1000.0);

    // Midpoint of viewport should map to midpoint of pixel range
    float x = vs.time_to_x(500.0, 0.0f, 800.0f);
    EXPECT_FLOAT_EQ(x, 400.0f);

    // Start of viewport
    x = vs.time_to_x(0.0, 0.0f, 800.0f);
    EXPECT_FLOAT_EQ(x, 0.0f);

    // End of viewport
    x = vs.time_to_x(1000.0, 0.0f, 800.0f);
    EXPECT_FLOAT_EQ(x, 800.0f);
}

TEST(ViewState, TimeToXWithOffset) {
    ViewState vs;
    vs.set_view_start_ts(0.0);
    vs.set_view_end_ts(1000.0);

    float x = vs.time_to_x(500.0, 200.0f, 800.0f);
    EXPECT_FLOAT_EQ(x, 600.0f);  // 200 + 400
}

TEST(ViewState, XToTime) {
    ViewState vs;
    vs.set_view_start_ts(0.0);
    vs.set_view_end_ts(1000.0);

    double t = vs.x_to_time(400.0f, 0.0f, 800.0f);
    EXPECT_DOUBLE_EQ(t, 500.0);
}

TEST(ViewState, TimeToXRoundTrip) {
    ViewState vs;
    vs.set_view_start_ts(12345.0);
    vs.set_view_end_ts(67890.0);

    double original_time = 40000.0;
    float x = vs.time_to_x(original_time, 100.0f, 1000.0f);
    double recovered = vs.x_to_time(x, 100.0f, 1000.0f);
    // Float intermediate in time_to_x limits precision
    EXPECT_NEAR(recovered, original_time, 0.01);
}

TEST(ViewState, ZoomToFit) {
    ViewState vs;
    vs.zoom_to_fit(100.0, 500.0);

    // Should add 2% padding on each side
    double range = 500.0 - 100.0;
    double padding = range * 0.02;
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 100.0 - padding);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 500.0 + padding);
}

TEST(ViewState, NavigateToEvent) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 1000.0;
    ev.dur = 200.0;

    vs.navigate_to_event(42, ev);

    EXPECT_EQ(vs.selected_event_idx(), 42);
    EXPECT_EQ(vs.pending_scroll_event_idx(), 42);
    // pad = max(200 * 0.5, 100) = 100
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 1000.0 - 100.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 1200.0 + 100.0);
}

TEST(ViewState, NavigateToEventCustomPadding) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 5000.0;
    ev.dur = 100.0;

    vs.navigate_to_event(7, ev, 2.0, 1000.0);

    EXPECT_EQ(vs.selected_event_idx(), 7);
    // pad = max(100 * 2.0, 1000) = 1000
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 5000.0 - 1000.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 5100.0 + 1000.0);
}

TEST(ViewState, NavigateToEventMinPadPreventsOverZoom) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 500.0;
    ev.dur = 0.0;  // instant event

    vs.navigate_to_event(3, ev);

    // pad = max(0 * 0.5, 100) = 100 (min_pad prevents zero-width viewport)
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 400.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 600.0);
}

// --- Range Selection ---

TEST(ViewState, RangeSelectionDefaults) {
    ViewState vs;
    EXPECT_FALSE(vs.has_range_selection());
    EXPECT_DOUBLE_EQ(vs.range_start_ts(), 0.0);
    EXPECT_DOUBLE_EQ(vs.range_end_ts(), 0.0);
}

TEST(ViewState, SetRangeSelection) {
    ViewState vs;
    vs.set_range_selection(100.0, 500.0);

    EXPECT_TRUE(vs.has_range_selection());
    EXPECT_DOUBLE_EQ(vs.range_start_ts(), 100.0);
    EXPECT_DOUBLE_EQ(vs.range_end_ts(), 500.0);
}

TEST(ViewState, SetRangeSelectionNormalizesOrder) {
    ViewState vs;
    // Drag right-to-left: end < start
    vs.set_range_selection(500.0, 100.0);

    EXPECT_TRUE(vs.has_range_selection());
    EXPECT_DOUBLE_EQ(vs.range_start_ts(), 100.0);
    EXPECT_DOUBLE_EQ(vs.range_end_ts(), 500.0);
}

TEST(ViewState, ClearRangeSelection) {
    ViewState vs;
    vs.set_range_selection(100.0, 500.0);
    vs.clear_range_selection();

    EXPECT_FALSE(vs.has_range_selection());
    EXPECT_DOUBLE_EQ(vs.range_start_ts(), 0.0);
    EXPECT_DOUBLE_EQ(vs.range_end_ts(), 0.0);
}

// --- Trace bounds clamping ---

TEST(ViewState, ClampViewToBoundsPreventsPanningPastEnd) {
    ViewState vs;
    vs.set_trace_bounds(100.0, 1000.0);
    // Try to pan past the end
    vs.set_view_range(900.0, 1100.0);
    // Should be clamped: end <= max_ts + 2% padding
    double pad = (1000.0 - 100.0) * 0.02;
    EXPECT_LE(vs.view_end_ts(), 1000.0 + pad + 0.001);
    EXPECT_DOUBLE_EQ(vs.view_end_ts() - vs.view_start_ts(), 200.0);
}

TEST(ViewState, ClampViewToBoundsPreventsPanningPastStart) {
    ViewState vs;
    vs.set_trace_bounds(100.0, 1000.0);
    // Try to pan before the start
    vs.set_view_range(0.0, 200.0);
    double pad = (1000.0 - 100.0) * 0.02;
    EXPECT_GE(vs.view_start_ts(), 100.0 - pad - 0.001);
    EXPECT_DOUBLE_EQ(vs.view_end_ts() - vs.view_start_ts(), 200.0);
}

TEST(ViewState, ClampViewToBoundsAllowsNormalRange) {
    ViewState vs;
    vs.set_trace_bounds(100.0, 1000.0);
    vs.set_view_range(200.0, 500.0);
    // Should be unchanged — well within bounds
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), 200.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 500.0);
}

TEST(ViewState, NoBoundsSetDoesNotClamp) {
    ViewState vs;
    // No trace bounds set — should not clamp
    vs.set_view_range(-5000.0, 50000.0);
    EXPECT_DOUBLE_EQ(vs.view_start_ts(), -5000.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts(), 50000.0);
}

// --- Range Stats ---

static TraceModel make_range_test_model() {
    TraceModel model;
    uint32_t name_a = model.intern_string("FuncA");
    uint32_t name_b = model.intern_string("FuncB");
    uint32_t cat = model.intern_string("test");

    auto& proc = model.get_or_create_process(1);
    auto& thread = proc.get_or_create_thread(1);

    // FuncA: 100-300 (dur=200)
    TraceEvent e0;
    e0.ts = 100.0;
    e0.dur = 200.0;
    e0.pid = 1;
    e0.tid = 1;
    e0.name_idx = name_a;
    e0.cat_idx = cat;
    e0.ph = Phase::Complete;
    model.add_event(e0);
    thread.event_indices.push_back(0);

    // FuncB: 200-400 (dur=200)
    TraceEvent e1;
    e1.ts = 200.0;
    e1.dur = 200.0;
    e1.pid = 1;
    e1.tid = 1;
    e1.name_idx = name_b;
    e1.cat_idx = cat;
    e1.ph = Phase::Complete;
    model.add_event(e1);
    thread.event_indices.push_back(1);

    // FuncA: 500-600 (dur=100)
    TraceEvent e2;
    e2.ts = 500.0;
    e2.dur = 100.0;
    e2.pid = 1;
    e2.tid = 1;
    e2.name_idx = name_a;
    e2.cat_idx = cat;
    e2.ph = Phase::Complete;
    model.add_event(e2);
    thread.event_indices.push_back(2);

    model.build_index();
    return model;
}

TEST(RangeStats, EmptyRange) {
    TraceModel model = make_range_test_model();
    auto stats = compute_range_stats(model, 700.0, 800.0);

    EXPECT_EQ(stats.total_events, 0u);
    EXPECT_TRUE(stats.summaries.empty());
    EXPECT_DOUBLE_EQ(stats.range_duration, 100.0);
}

TEST(RangeStats, FullRange) {
    TraceModel model = make_range_test_model();
    auto stats = compute_range_stats(model, 0.0, 700.0);

    EXPECT_EQ(stats.total_events, 3u);
    EXPECT_EQ(stats.summaries.size(), 2u);
    // FuncA: 200 + 100 = 300 total, FuncB: 200 total
    // Sorted by total_dur descending, FuncA should be first
    EXPECT_EQ(model.get_string(stats.summaries[0].name_idx), "FuncA");
    EXPECT_NEAR(stats.summaries[0].total_dur, 300.0, 0.01);
    EXPECT_EQ(stats.summaries[0].count, 2u);

    EXPECT_EQ(model.get_string(stats.summaries[1].name_idx), "FuncB");
    EXPECT_NEAR(stats.summaries[1].total_dur, 200.0, 0.01);
    EXPECT_EQ(stats.summaries[1].count, 1u);
}

TEST(RangeStats, PartialOverlapClampsContribution) {
    TraceModel model = make_range_test_model();
    // Range 250-350: FuncA(100-300) contributes 50, FuncB(200-400) contributes 100
    auto stats = compute_range_stats(model, 250.0, 350.0);

    EXPECT_EQ(stats.total_events, 2u);
    EXPECT_DOUBLE_EQ(stats.range_duration, 100.0);

    // FuncB contributes more (100 vs 50), should be first
    EXPECT_EQ(model.get_string(stats.summaries[0].name_idx), "FuncB");
    EXPECT_NEAR(stats.summaries[0].total_dur, 100.0, 0.01);

    EXPECT_EQ(model.get_string(stats.summaries[1].name_idx), "FuncA");
    EXPECT_NEAR(stats.summaries[1].total_dur, 50.0, 0.01);
}

TEST(RangeStats, AggregatesMinMaxAvg) {
    TraceModel model = make_range_test_model();
    auto stats = compute_range_stats(model, 0.0, 700.0);

    // FuncA has two instances: 200 and 100
    const auto& func_a = stats.summaries[0];
    EXPECT_EQ(model.get_string(func_a.name_idx), "FuncA");
    EXPECT_NEAR(func_a.min_dur, 100.0, 0.01);
    EXPECT_NEAR(func_a.max_dur, 200.0, 0.01);
    EXPECT_NEAR(func_a.avg_dur(), 150.0, 0.01);
}

TEST(RangeStats, LongestIdxTracksByActualDuration) {
    TraceModel model = make_range_test_model();
    // FuncA event 0 is 200us (100-300), event 2 is 100us (500-600)
    // Even with a partial range, longest_idx should point to the 200us event
    auto stats = compute_range_stats(model, 250.0, 700.0);

    const auto* func_a = &stats.summaries[0];
    if (model.get_string(func_a->name_idx) != "FuncA") func_a = &stats.summaries[1];
    EXPECT_EQ(model.get_string(func_a->name_idx), "FuncA");
    // Event 0 has dur=200, event 2 has dur=100 — longest_idx should be event 0
    EXPECT_EQ(func_a->longest_idx, 0u);
    EXPECT_DOUBLE_EQ(model.events()[func_a->longest_idx].dur, 200.0);
}
