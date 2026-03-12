#include <gtest/gtest.h>
#include "ui/format_time.h"
#include <cstring>

static std::string fmt(double us) {
    char buf[64];
    format_time(us, buf, sizeof(buf));
    return buf;
}

TEST(FormatTime, Nanoseconds) {
    EXPECT_EQ(fmt(0.5), "500.0 ns");
    EXPECT_EQ(fmt(0.001), "1.0 ns");
    EXPECT_EQ(fmt(0.1234), "123.4 ns");
}

TEST(FormatTime, Microseconds) {
    EXPECT_EQ(fmt(1.0), "1.000 us");
    EXPECT_EQ(fmt(999.999), "999.999 us");
    EXPECT_EQ(fmt(50.5), "50.500 us");
}

TEST(FormatTime, Milliseconds) {
    EXPECT_EQ(fmt(1000.0), "1.000 ms");
    EXPECT_EQ(fmt(500000.0), "500.000 ms");
    EXPECT_EQ(fmt(1500.0), "1.500 ms");
}

TEST(FormatTime, Seconds) {
    EXPECT_EQ(fmt(1000000.0), "1.000 s");
    EXPECT_EQ(fmt(5500000.0), "5.500 s");
    EXPECT_EQ(fmt(60000000.0), "60.000 s");
}

TEST(FormatTime, Negative) {
    EXPECT_EQ(fmt(-0.5), "-500.0 ns");
    EXPECT_EQ(fmt(-50.0), "-50.000 us");
    EXPECT_EQ(fmt(-5000.0), "-5.000 ms");
    EXPECT_EQ(fmt(-5000000.0), "-5.000 s");
}

TEST(FormatTime, Zero) {
    EXPECT_EQ(fmt(0.0), "0.0 ns");
}

TEST(FormatTime, BoundaryValues) {
    // Just below 1 us
    EXPECT_EQ(fmt(0.999), "999.0 ns");
    // Exactly 1 us
    EXPECT_EQ(fmt(1.0), "1.000 us");
    // Just below 1 ms
    EXPECT_EQ(fmt(999.999), "999.999 us");
    // Exactly 1 ms
    EXPECT_EQ(fmt(1000.0), "1.000 ms");
    // Just below 1 s
    EXPECT_EQ(fmt(999999.0), "999.999 ms");
    // Exactly 1 s
    EXPECT_EQ(fmt(1000000.0), "1.000 s");
}

static std::string fmt_ruler(double us, double tick_interval) {
    char buf[64];
    format_ruler_time(us, tick_interval, buf, sizeof(buf));
    return buf;
}

TEST(FormatRulerTime, SecondTicks) {
    EXPECT_EQ(fmt_ruler(2000000.0, 1000000.0), "2.0 s");
    EXPECT_EQ(fmt_ruler(5000000.0, 10000000.0), "5 s");
}

TEST(FormatRulerTime, MillisecondTicks) {
    EXPECT_EQ(fmt_ruler(1500000.0, 500000.0), "1.5 s");
    EXPECT_EQ(fmt_ruler(500.0, 100000.0), "0.5 ms");
}

TEST(FormatRulerTime, MicrosecondTicks) {
    EXPECT_EQ(fmt_ruler(50.0, 10.0), "50.0 us");
    EXPECT_EQ(fmt_ruler(1500.0, 100.0), "1.5 ms");
}
