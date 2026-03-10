#include <gtest/gtest.h>
#include "model/trace_event.h"

TEST(PhaseFromChar, MapsKnownPhases) {
    EXPECT_EQ(phase_from_char('B'), Phase::DurationBegin);
    EXPECT_EQ(phase_from_char('E'), Phase::DurationEnd);
    EXPECT_EQ(phase_from_char('X'), Phase::Complete);
    EXPECT_EQ(phase_from_char('i'), Phase::Instant);
    EXPECT_EQ(phase_from_char('I'), Phase::Instant);
    EXPECT_EQ(phase_from_char('C'), Phase::Counter);
    EXPECT_EQ(phase_from_char('b'), Phase::AsyncBegin);
    EXPECT_EQ(phase_from_char('e'), Phase::AsyncEnd);
    EXPECT_EQ(phase_from_char('n'), Phase::AsyncInstant);
    EXPECT_EQ(phase_from_char('s'), Phase::FlowStart);
    EXPECT_EQ(phase_from_char('t'), Phase::FlowStep);
    EXPECT_EQ(phase_from_char('f'), Phase::FlowEnd);
    EXPECT_EQ(phase_from_char('M'), Phase::Metadata);
    EXPECT_EQ(phase_from_char('N'), Phase::ObjectCreated);
    EXPECT_EQ(phase_from_char('O'), Phase::ObjectSnapshot);
    EXPECT_EQ(phase_from_char('D'), Phase::ObjectDestroyed);
    EXPECT_EQ(phase_from_char('P'), Phase::Sample);
    EXPECT_EQ(phase_from_char('R'), Phase::Mark);
}

TEST(PhaseFromChar, UnknownCharsReturnUnknown) {
    EXPECT_EQ(phase_from_char('Z'), Phase::Unknown);
    EXPECT_EQ(phase_from_char('x'), Phase::Unknown);
    EXPECT_EQ(phase_from_char('\0'), Phase::Unknown);
    EXPECT_EQ(phase_from_char('1'), Phase::Unknown);
}

TEST(TraceEvent, EndTs) {
    TraceEvent ev;
    ev.ts = 100.0;
    ev.dur = 50.0;
    EXPECT_DOUBLE_EQ(ev.end_ts(), 150.0);
}

TEST(TraceEvent, DefaultValues) {
    TraceEvent ev;
    EXPECT_EQ(ev.name_idx, 0u);
    EXPECT_EQ(ev.cat_idx, 0u);
    EXPECT_EQ(ev.ph, Phase::Unknown);
    EXPECT_DOUBLE_EQ(ev.ts, 0.0);
    EXPECT_DOUBLE_EQ(ev.dur, 0.0);
    EXPECT_EQ(ev.pid, 0u);
    EXPECT_EQ(ev.tid, 0u);
    EXPECT_EQ(ev.id, 0u);
    EXPECT_EQ(ev.args_idx, UINT32_MAX);
    EXPECT_EQ(ev.depth, 0);
    EXPECT_FALSE(ev.is_end_event);
}
