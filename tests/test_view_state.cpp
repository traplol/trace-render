#include <gtest/gtest.h>
#include "ui/view_state.h"
#include <cmath>

TEST(ViewState, DefaultValues) {
    ViewState vs;
    EXPECT_DOUBLE_EQ(vs.view_start_ts, 0.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts, 1000.0);
    EXPECT_EQ(vs.selected_event_idx, -1);
    EXPECT_TRUE(vs.show_flows);
}

TEST(ViewState, TimeToX) {
    ViewState vs;
    vs.view_start_ts = 0.0;
    vs.view_end_ts = 1000.0;

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
    vs.view_start_ts = 0.0;
    vs.view_end_ts = 1000.0;

    float x = vs.time_to_x(500.0, 200.0f, 800.0f);
    EXPECT_FLOAT_EQ(x, 600.0f);  // 200 + 400
}

TEST(ViewState, XToTime) {
    ViewState vs;
    vs.view_start_ts = 0.0;
    vs.view_end_ts = 1000.0;

    double t = vs.x_to_time(400.0f, 0.0f, 800.0f);
    EXPECT_DOUBLE_EQ(t, 500.0);
}

TEST(ViewState, TimeToXRoundTrip) {
    ViewState vs;
    vs.view_start_ts = 12345.0;
    vs.view_end_ts = 67890.0;

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
    EXPECT_DOUBLE_EQ(vs.view_start_ts, 100.0 - padding);
    EXPECT_DOUBLE_EQ(vs.view_end_ts, 500.0 + padding);
}

TEST(ViewState, NavigateToEvent) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 1000.0;
    ev.dur = 200.0;

    vs.navigate_to_event(42, ev);

    EXPECT_EQ(vs.selected_event_idx, 42);
    EXPECT_EQ(vs.pending_scroll_event_idx, 42);
    // pad = max(200 * 0.5, 100) = 100
    EXPECT_DOUBLE_EQ(vs.view_start_ts, 1000.0 - 100.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts, 1200.0 + 100.0);
}

TEST(ViewState, NavigateToEventCustomPadding) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 5000.0;
    ev.dur = 100.0;

    vs.navigate_to_event(7, ev, 2.0, 1000.0);

    EXPECT_EQ(vs.selected_event_idx, 7);
    // pad = max(100 * 2.0, 1000) = 1000
    EXPECT_DOUBLE_EQ(vs.view_start_ts, 5000.0 - 1000.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts, 5100.0 + 1000.0);
}

TEST(ViewState, NavigateToEventMinPadPreventsOverZoom) {
    ViewState vs;
    TraceEvent ev;
    ev.ts = 500.0;
    ev.dur = 0.0;  // instant event

    vs.navigate_to_event(3, ev);

    // pad = max(0 * 0.5, 100) = 100 (min_pad prevents zero-width viewport)
    EXPECT_DOUBLE_EQ(vs.view_start_ts, 400.0);
    EXPECT_DOUBLE_EQ(vs.view_end_ts, 600.0);
}
