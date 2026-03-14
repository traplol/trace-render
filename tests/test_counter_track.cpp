#include <gtest/gtest.h>
#include "ui/counter_track.h"

static CounterSeries make_series(std::vector<std::pair<double, double>> points) {
    CounterSeries s;
    s.name = "test";
    s.points = std::move(points);
    return s;
}

// --- counter_lookup_value tests ---

TEST(CounterLookup, EmptySeries) {
    CounterSeries s = make_series({});
    double ts, val;
    EXPECT_FALSE(counter_lookup_value(s, 100.0, ts, val));
}

TEST(CounterLookup, BeforeFirstPoint) {
    CounterSeries s = make_series({{10.0, 5.0}, {20.0, 10.0}});
    double ts, val;
    EXPECT_FALSE(counter_lookup_value(s, 5.0, ts, val));
}

TEST(CounterLookup, ExactlyAtFirstPoint) {
    CounterSeries s = make_series({{10.0, 5.0}, {20.0, 10.0}});
    double ts, val;
    EXPECT_TRUE(counter_lookup_value(s, 10.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 10.0);
    EXPECT_DOUBLE_EQ(val, 5.0);
}

TEST(CounterLookup, BetweenPoints) {
    CounterSeries s = make_series({{10.0, 5.0}, {20.0, 10.0}, {30.0, 15.0}});
    double ts, val;
    // Between first and second — should return first point's value (step function)
    EXPECT_TRUE(counter_lookup_value(s, 15.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 10.0);
    EXPECT_DOUBLE_EQ(val, 5.0);

    // Between second and third
    EXPECT_TRUE(counter_lookup_value(s, 25.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 20.0);
    EXPECT_DOUBLE_EQ(val, 10.0);
}

TEST(CounterLookup, ExactlyAtSecondPoint) {
    CounterSeries s = make_series({{10.0, 5.0}, {20.0, 10.0}});
    double ts, val;
    EXPECT_TRUE(counter_lookup_value(s, 20.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 20.0);
    EXPECT_DOUBLE_EQ(val, 10.0);
}

TEST(CounterLookup, AfterLastPoint) {
    CounterSeries s = make_series({{10.0, 5.0}, {20.0, 10.0}});
    double ts, val;
    EXPECT_TRUE(counter_lookup_value(s, 999.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 20.0);
    EXPECT_DOUBLE_EQ(val, 10.0);
}

TEST(CounterLookup, SinglePoint) {
    CounterSeries s = make_series({{42.0, 7.0}});
    double ts, val;

    EXPECT_FALSE(counter_lookup_value(s, 41.0, ts, val));

    EXPECT_TRUE(counter_lookup_value(s, 42.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 42.0);
    EXPECT_DOUBLE_EQ(val, 7.0);

    EXPECT_TRUE(counter_lookup_value(s, 100.0, ts, val));
    EXPECT_DOUBLE_EQ(ts, 42.0);
    EXPECT_DOUBLE_EQ(val, 7.0);
}

// --- merge_counter_points tests ---

// Helper: view [0, 1000] mapped to track_x=0, track_w=1000 gives 1:1 time-to-pixel.
static std::vector<MergedCounterSegment> merge(std::vector<std::pair<double, double>> points, double view_start = 0.0,
                                               double view_end = 1000.0, float track_x = 0.0f,
                                               float track_w = 1000.0f) {
    return merge_counter_points(points, view_start, view_end, track_x, track_w);
}

TEST(MergeCounterPoints, EmptyPoints) {
    auto segs = merge({});
    EXPECT_TRUE(segs.empty());
}

TEST(MergeCounterPoints, SinglePoint) {
    auto segs = merge({{500.0, 42.0}});
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].point_count, 1);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 42.0);
    EXPECT_DOUBLE_EQ(segs[0].min_val, 42.0);
    EXPECT_DOUBLE_EQ(segs[0].max_val, 42.0);
}

TEST(MergeCounterPoints, DistinctPixels) {
    // Points at different pixel columns — no merging
    auto segs = merge({{100.0, 1.0}, {200.0, 2.0}, {300.0, 3.0}});
    ASSERT_EQ(segs.size(), 3u);
    for (auto& s : segs) EXPECT_EQ(s.point_count, 1);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 1.0);
    EXPECT_DOUBLE_EQ(segs[1].last_val, 2.0);
    EXPECT_DOUBLE_EQ(segs[2].last_val, 3.0);
}

TEST(MergeCounterPoints, MergesSamePixel) {
    // 3 points at the same pixel column (all map to pixel 100)
    auto segs = merge({{100.0, 5.0}, {100.3, 10.0}, {100.7, 3.0}});
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].point_count, 3);
    EXPECT_DOUBLE_EQ(segs[0].min_val, 3.0);
    EXPECT_DOUBLE_EQ(segs[0].max_val, 10.0);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 3.0);
}

TEST(MergeCounterPoints, MixedMergedAndSingle) {
    // Two points at pixel 100, one at pixel 300
    auto segs = merge({{100.0, 5.0}, {100.5, 15.0}, {300.0, 7.0}});
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].point_count, 2);
    EXPECT_DOUBLE_EQ(segs[0].min_val, 5.0);
    EXPECT_DOUBLE_EQ(segs[0].max_val, 15.0);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 15.0);
    EXPECT_EQ(segs[1].point_count, 1);
    EXPECT_DOUBLE_EQ(segs[1].last_val, 7.0);
}

TEST(MergeCounterPoints, IncludesPointBeforeView) {
    // Point at -50 is before view_start=0 but should be included for continuity
    auto segs = merge({{-50.0, 1.0}, {500.0, 2.0}});
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 1.0);
    EXPECT_DOUBLE_EQ(segs[1].last_val, 2.0);
}

TEST(MergeCounterPoints, IncludesPointPastViewEnd) {
    // Point at 1500 is past view_end=1000 but one past-end point should be included
    auto segs = merge({{500.0, 1.0}, {1500.0, 2.0}});
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 1.0);
    EXPECT_DOUBLE_EQ(segs[1].last_val, 2.0);
}

TEST(MergeCounterPoints, StopAfterFirstPastEnd) {
    // Only first past-end point should be included
    auto segs = merge({{500.0, 1.0}, {1500.0, 2.0}, {2000.0, 3.0}});
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_DOUBLE_EQ(segs[1].last_val, 2.0);
}

TEST(MergeCounterPoints, AllPointsBeforeView) {
    // Both points are before view_start; lower_bound returns end(), backs up to -100 only
    auto segs = merge({{-200.0, 1.0}, {-100.0, 2.0}});
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 2.0);
}

TEST(MergeCounterPoints, DenseDataMerges) {
    // 100 points in 1 pixel range — should merge into one segment
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < 100; i++) {
        pts.push_back({500.0 + i * 0.001, (double)i});
    }
    auto segs = merge(pts);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].point_count, 100);
    EXPECT_DOUBLE_EQ(segs[0].min_val, 0.0);
    EXPECT_DOUBLE_EQ(segs[0].max_val, 99.0);
    EXPECT_DOUBLE_EQ(segs[0].last_val, 99.0);
}

TEST(MergeCounterPoints, ZoomedOutMergesMultipleBuckets) {
    // View is 0-1000 mapped to 100px — each pixel covers 10 time units.
    // Points at time 0,1,2,...,99 should merge into ~10 buckets.
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < 100; i++) {
        pts.push_back({(double)i, (double)(i % 10)});
    }
    auto segs = merge_counter_points(pts, 0.0, 1000.0, 0.0f, 100.0f);
    // All 100 points map to pixels 0-9 (10 buckets)
    ASSERT_EQ(segs.size(), 10u);
    for (auto& s : segs) {
        EXPECT_EQ(s.point_count, 10);
    }
}
