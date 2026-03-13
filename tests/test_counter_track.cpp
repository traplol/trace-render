#include <gtest/gtest.h>
#include "ui/counter_track.h"

static CounterSeries make_series(std::vector<std::pair<double, double>> points) {
    CounterSeries s;
    s.name = "test";
    s.points = std::move(points);
    return s;
}

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
