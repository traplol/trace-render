#include <gtest/gtest.h>
#include "model/trace_model.h"
#include "parser/native_heap.h"
#include "parser/profile_io.h"
#include <limits>

namespace {
ProfileData capture() {
    ProfileData profile;
    profile.capture_start_ts = 0;
    profile.capture_end_ts = 100;
    profile.processes = {{"first", 7, 0, std::nullopt}};
    return profile;
}

NativeHeapEvent event(NativeHeapEventKind kind, double ts, uint64_t address = 0, uint64_t size = 0,
                      const std::string& stack = "leaf") {
    NativeHeapEvent result;
    result.kind = kind;
    result.ts_us = ts;
    result.raw_qpc = static_cast<uint64_t>(ts * 10);
    result.process_id = "first";
    result.tid = 11;
    result.heap_handle = 0x100;
    result.address = address;
    result.size_bytes = size;
    result.stack_frame_id = stack;
    return result;
}

void add_frame(TraceModel& model, const std::string& id, const std::string& parent, const std::string& symbol) {
    StackFrame frame;
    frame.id_idx = model.intern_string(id);
    frame.name_idx = model.intern_string("same display name");
    frame.symbol_id = model.intern_string(symbol);
    if (!parent.empty()) frame.parent_id = model.intern_string(parent);
    model.add_stack_frame(frame);
}

TraceModel model_for(ProfileData profile) {
    TraceModel model;
    model.intern_string("");
    add_frame(model, "root", "", "main");
    add_frame(model, "leaf", "root", "allocate");
    add_frame(model, "other", "root", "other-symbol");
    add_frame(model, "recursive", "leaf", "allocate");
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}

void expect_total(const TraceModel& model, double ts, uint64_t bytes, uint64_t count) {
    const auto result = model.query_outstanding_memory(ts);
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.total.known.bytes, bytes);
    EXPECT_EQ(result.total.known.count, count);
    EXPECT_EQ(result.total.uncertain.bytes, 0u);
    EXPECT_EQ(result.total.uncertain.count, 0u);
}
}  // namespace

TEST(NativeMemory, LifetimesHeapReuseAndProcessReuseHaveDistinctIdentities) {
    auto profile = capture();
    profile.processes[0].end_ts = 20;
    profile.processes.push_back({"second", 7, 20, std::nullopt});
    std::vector<NativeHeapEvent> events = {event(NativeHeapEventKind::Create, 0),
                                           event(NativeHeapEventKind::Allocate, 1, 0x200, 20),
                                           event(NativeHeapEventKind::Allocate, 2, 0x300, 30),
                                           event(NativeHeapEventKind::Free, 3, 0x200),
                                           event(NativeHeapEventKind::Allocate, 4, 0x200, 40),
                                           event(NativeHeapEventKind::Destroy, 10),
                                           event(NativeHeapEventKind::Create, 11),
                                           event(NativeHeapEventKind::Allocate, 12, 0x200, 50),
                                           event(NativeHeapEventKind::Create, 20),
                                           event(NativeHeapEventKind::Allocate, 20, 0x200, 60)};
    events[3].tid = 999;  // Cross-thread free must charge the original allocation.
    events[3].stack_frame_id = "other";
    events[8].process_id = events[9].process_id = "second";
    build_native_allocations(events, profile, true);
    ASSERT_EQ(profile.allocations.size(), 5u);
    ASSERT_EQ(profile.heaps.size(), 3u);
    EXPECT_NE(profile.allocations[0].id, profile.allocations[2].id);
    EXPECT_NE(profile.allocations[2].heap_id, profile.allocations[3].heap_id);
    EXPECT_NE(profile.allocations[3].heap_id, profile.allocations[4].heap_id);
    EXPECT_EQ(profile.allocations[0].stack_frame_id, "leaf");
    EXPECT_EQ(profile.allocations[3].freed_ts, 20);
    auto model = model_for(std::move(profile));
    expect_total(model, 0, 0, 0);
    expect_total(model, 1, 20, 1);
    expect_total(model, 3, 30, 1);
    expect_total(model, 4, 70, 2);
    expect_total(model, 10, 0, 0);
    expect_total(model, 19, 50, 1);
    expect_total(model, 20, 60, 1);
    expect_total(model, 100, 60, 1);
    EXPECT_FALSE(model.query_outstanding_memory(101).error.empty());
}

TEST(NativeMemory, ReallocationSummaryPreservesObservedNewOriginAndResizeStartsGeneration) {
    auto profile = capture();
    std::vector<NativeHeapEvent> events = {event(NativeHeapEventKind::Create, 0),
                                           event(NativeHeapEventKind::Allocate, 1, 0x200, 308),
                                           event(NativeHeapEventKind::Allocate, 2, 0x300, 564, "other"),
                                           event(NativeHeapEventKind::Free, 3, 0x200),
                                           event(NativeHeapEventKind::Reallocate, 4, 0x300, 564, "recursive"),
                                           event(NativeHeapEventKind::Reallocate, 5, 0x300, 600),
                                           event(NativeHeapEventKind::Reallocate, 6, 0, 1000)};
    events[4].old_address = 0x200;
    events[4].old_size_bytes = 308;
    events[5].old_address = events[6].old_address = 0x300;
    events[5].old_size_bytes = 564;
    events[6].old_size_bytes = 600;
    build_native_allocations(events, profile, true);
    ASSERT_EQ(profile.allocations.size(), 3u);
    EXPECT_EQ(profile.allocations[1].allocated_ts, 2);
    EXPECT_EQ(profile.allocations[1].stack_frame_id, "other");
    EXPECT_EQ(profile.allocations[1].freed_ts, 5);
    EXPECT_EQ(profile.allocations[2].allocated_ts, 5);
    EXPECT_EQ(profile.allocations[2].stack_frame_id, "leaf");
    auto model = model_for(std::move(profile));
    expect_total(model, 2, 872, 2);
    expect_total(model, 3, 564, 1);
    expect_total(model, 5, 600, 1);
    expect_total(model, 100, 600, 1);
    auto before_resize = model.query_outstanding_memory(6, {{1, 5}});
    EXPECT_EQ(before_resize.total.known.count, 0u);
    auto resized = model.query_outstanding_memory(6, {{5, 6}});
    EXPECT_EQ(resized.total.known.bytes, 600u);
}

TEST(NativeMemory, OutstandingAndBirthRangeAreDifferentQueriesWithHalfOpenBoundaries) {
    auto profile = capture();
    std::vector<NativeHeapEvent> events = {
        event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 10, 1, 11),
        event(NativeHeapEventKind::Allocate, 20, 2, 22), event(NativeHeapEventKind::Allocate, 30, 3, 33),
        event(NativeHeapEventKind::Free, 40, 2)};
    build_native_allocations(events, profile, true);
    auto model = model_for(std::move(profile));
    expect_total(model, 30, 66, 3);
    auto cohort = model.query_outstanding_memory(30, {{20, 30}});
    EXPECT_EQ(cohort.total.known.bytes, 22u);
    EXPECT_EQ(cohort.total.known.count, 1u);
    auto at_release = model.query_outstanding_memory(40, {{20, 30}});
    EXPECT_EQ(at_release.total.known.count, 0u);
    EXPECT_EQ(model.query_outstanding_memory(30, {{20, 20}}).total.known.count, 0u);
    EXPECT_FALSE(model.query_outstanding_memory(30, {{30, 20}}).error.empty());
    EXPECT_FALSE(model.query_outstanding_memory(std::numeric_limits<double>::quiet_NaN()).error.empty());
}

TEST(NativeMemory, MissingFreesStacksAndCoverageRemainVisibleWithoutInventingZeroByteAllocations) {
    auto profile = capture();
    std::vector<NativeHeapEvent> events = {event(NativeHeapEventKind::Free, 1, 0x200),
                                           event(NativeHeapEventKind::Allocate, 2, 0x300, 50, "")};
    build_native_allocations(events, profile, false);
    ASSERT_EQ(profile.allocations.size(), 1u);
    EXPECT_EQ(profile.allocations[0].end_state, AllocationEnd::Unknown);
    EXPECT_FALSE(profile.allocations[0].freed_ts);
    EXPECT_TRUE(profile.quality.incomplete_capture);
    EXPECT_FALSE(profile.quality.warnings.empty());
    auto model = model_for(std::move(profile));
    auto result = model.query_outstanding_memory(50);
    EXPECT_EQ(result.total.known.bytes, 0u);
    EXPECT_EQ(result.total.uncertain.bytes, 50u);
    ASSERT_EQ(result.stacks.size(), 1u);
    EXPECT_EQ(result.stacks[0].frame_index, -1);
    EXPECT_TRUE(result.incomplete);
}

TEST(NativeMemory, MissingAllocationEndDoesNotBecomeAnExactFreeAtAddressReuse) {
    auto profile = capture();
    build_native_allocations({event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 100, 10),
                              event(NativeHeapEventKind::Allocate, 2, 100, 20)},
                             profile, true);
    ASSERT_EQ(profile.allocations.size(), 2u);
    EXPECT_EQ(profile.allocations[0].end_state, AllocationEnd::Unknown);
    EXPECT_FALSE(profile.allocations[0].freed_ts);
    auto model = model_for(std::move(profile));
    auto result = model.query_outstanding_memory(10);
    EXPECT_EQ(result.total.known.bytes, 20u);
    EXPECT_EQ(result.total.uncertain.bytes, 10u);
    EXPECT_TRUE(result.incomplete);
}

TEST(NativeMemory, SeparateHeapsOwnTheSameAddressAndUnobservedDestructionIsNotAnExactFree) {
    auto profile = capture();
    auto second_heap = event(NativeHeapEventKind::Allocate, 2, 100, 20);
    second_heap.heap_handle = 0x101;
    build_native_allocations(
        {event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 100, 10), second_heap,
         event(NativeHeapEventKind::Create, 3), event(NativeHeapEventKind::Allocate, 4, 100, 30),
         event(NativeHeapEventKind::Free, 5, 100)},
        profile, true);
    ASSERT_EQ(profile.allocations.size(), 3u);
    ASSERT_EQ(profile.heaps.size(), 3u);
    EXPECT_EQ(profile.allocations[0].end_state, AllocationEnd::Unknown);
    EXPECT_FALSE(profile.allocations[0].freed_ts);
    EXPECT_EQ(profile.heaps[0].end_ts, 3);
    auto model = model_for(std::move(profile));
    auto earlier = model.query_outstanding_memory(2);
    EXPECT_EQ(earlier.total.known.bytes, 20u);
    EXPECT_EQ(earlier.total.uncertain.bytes, 10u);
    expect_total(model, 3, 20, 1);
    expect_total(model, 4, 50, 2);
    expect_total(model, 5, 20, 1);
}

TEST(NativeMemory, UnpairedMovedReallocAndMissingCoverageEndDoNotClaimLiveness) {
    auto profile = capture();
    auto moved = event(NativeHeapEventKind::Reallocate, 3, 200, 20);
    moved.old_address = 100;
    moved.old_size_bytes = 10;
    build_native_allocations(
        {event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 100, 10), moved}, profile,
        true);
    ASSERT_EQ(profile.allocations.size(), 1u);
    EXPECT_EQ(profile.allocations[0].end_state, AllocationEnd::Unknown);
    EXPECT_TRUE(profile.quality.incomplete_capture);
    auto model = model_for(std::move(profile));
    auto result = model.query_outstanding_memory(10);
    EXPECT_EQ(result.total.known.count, 0u);
    EXPECT_EQ(result.total.uncertain.bytes, 10u);

    profile = capture();
    profile.capture_end_ts.reset();
    build_native_allocations({event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 100, 10)},
                             profile, true);
    model = model_for(std::move(profile));
    result = model.query_outstanding_memory(10);
    EXPECT_EQ(result.total.known.count, 0u);
    EXPECT_EQ(result.total.uncertain.bytes, 10u);
    EXPECT_TRUE(result.incomplete);
}

TEST(NativeMemory, EventsOutsideCaptureCannotExtendItsCoverage) {
    auto profile = capture();
    build_native_allocations({event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 100, 10),
                              event(NativeHeapEventKind::Free, 101, 100)},
                             profile, true);
    EXPECT_FALSE(profile.allocations[0].freed_ts);
    EXPECT_EQ(profile.allocations[0].end_state, AllocationEnd::Unknown);
    auto model = model_for(std::move(profile));
    auto result = model.query_outstanding_memory(100);
    EXPECT_EQ(result.total.known.bytes, 0u);
    EXPECT_EQ(result.total.uncertain.bytes, 10u);
    EXPECT_FALSE(model.query_outstanding_memory(-1).error.empty());
}

TEST(NativeMemory, GroupsBySymbolNotDisplayNameAndCountsRecursiveFunctionsOnce) {
    auto profile = capture();
    build_native_allocations(
        {event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 1, 10, "recursive"),
         event(NativeHeapEventKind::Allocate, 2, 2, 20, "other"), event(NativeHeapEventKind::Allocate, 3, 3, 30)},
        profile, true);
    auto model = model_for(std::move(profile));
    auto result = model.query_outstanding_memory(10);
    ASSERT_EQ(result.stacks.size(), 3u);
    ASSERT_EQ(result.functions.size(), 3u);
    for (const auto& function : result.functions) {
        const auto& frame = model.stack_frames()[function.frame_index];
        const auto& symbol = model.get_string(frame.symbol_id);
        if (symbol == "main") {
            EXPECT_EQ(function.inclusive.known.bytes, 60u);
            EXPECT_EQ(function.exclusive.known.bytes, 0u);
        } else if (symbol == "allocate") {
            EXPECT_EQ(function.inclusive.known.bytes, 40u);
            EXPECT_EQ(function.inclusive.known.count, 2u);
            EXPECT_EQ(function.exclusive.known.bytes, 40u);
        } else {
            EXPECT_EQ(symbol, "other-symbol");
            EXPECT_EQ(function.inclusive.known.bytes, 20u);
        }
    }
}

TEST(NativeMemory, FunctionIdentityKeepsModuleLoadsAndUnknownFramesSeparate) {
    auto profile = capture();
    TraceModel model;
    std::vector<NativeHeapEvent> events = {event(NativeHeapEventKind::Create, 0)};
    for (int i = 0; i < 5; ++i) {
        const auto id = "frame-" + std::to_string(i);
        StackFrame frame;
        frame.id_idx = model.intern_string(id);
        frame.name_idx = model.intern_string("same display name");
        frame.address = i < 2 ? 100 : 200;
        if (i < 4) frame.module_id = model.intern_string(i == 1 ? "load-b" : "load-a");
        if (i < 2) frame.symbol_id = model.intern_string("same function id");
        model.add_stack_frame(frame);
        events.push_back(event(NativeHeapEventKind::Allocate, i + 1, i + 1, 10, id));
    }
    build_native_allocations(events, profile, true);
    model.set_profile(std::move(profile));
    model.build_index();
    const auto result = model.query_outstanding_memory(10);
    EXPECT_EQ(result.stacks.size(), 5u);
    ASSERT_EQ(result.functions.size(), 4u);
    uint64_t grouped_bytes = 0;
    size_t shared_raw_address = 0;
    for (const auto& function : result.functions) {
        grouped_bytes += function.exclusive.known.bytes;
        if (function.exclusive.known.count == 2) {
            ++shared_raw_address;
            const auto& frame = model.stack_frames()[function.frame_index];
            EXPECT_EQ(frame.address, 200);
            EXPECT_EQ(model.get_string(frame.module_id), "load-a");
        }
    }
    EXPECT_EQ(grouped_bytes, 50u);
    EXPECT_EQ(shared_raw_address, 1u);
}

TEST(NativeMemory, LossMakesMatchedLifetimesUncertainAndUnknownBirthCannotEnterCohort) {
    auto profile = capture();
    profile.quality.lost_events = 1;
    build_native_allocations({event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 1, 20),
                              event(NativeHeapEventKind::Free, 10, 1)},
                             profile, false);
    auto unknown_birth = profile.allocations[0];
    unknown_birth.id = "unknown-birth";
    unknown_birth.allocated_ts.reset();
    unknown_birth.size_bytes = 30;
    profile.allocations.push_back(unknown_birth);
    auto model = model_for(std::move(profile));
    auto all = model.query_outstanding_memory(5);
    EXPECT_EQ(all.total.known.bytes, 0u);
    EXPECT_EQ(all.total.uncertain.bytes, 50u);
    auto cohort = model.query_outstanding_memory(5, {{0, 5}});
    EXPECT_EQ(cohort.total.uncertain.bytes, 20u);
    EXPECT_EQ(cohort.unknown_birth_count, 1u);
}

TEST(NativeMemory, QuerySurvivesProfileRoundTripAndDetectsByteOverflow) {
    auto profile = capture();
    build_native_allocations(
        {event(NativeHeapEventKind::Create, 0), event(NativeHeapEventKind::Allocate, 1, 1, 10),
         event(NativeHeapEventKind::Allocate, 2, 2, 20, "other"), event(NativeHeapEventKind::Free, 5, 1)},
        profile, true);
    auto original = model_for(profile);
    std::string data, error;
    ASSERT_TRUE(serialize_profile(original, data, error)) << error;
    TraceModel restored;
    ASSERT_TRUE(read_profile(data, restored, error)) << error;
    for (double ts : {0, 1, 2, 5, 100}) {
        const auto before = original.query_outstanding_memory(ts, {{0, 3}});
        const auto after = restored.query_outstanding_memory(ts, {{0, 3}});
        EXPECT_EQ(after.total.known.bytes, before.total.known.bytes);
        EXPECT_EQ(after.total.known.count, before.total.known.count);
        EXPECT_EQ(after.functions.size(), before.functions.size());
    }
    profile.allocations[0].size_bytes = UINT64_MAX;
    auto oversized = model_for(profile);
    ASSERT_TRUE(serialize_profile(oversized, data, error)) << error;
    ASSERT_TRUE(read_profile(data, restored, error)) << error;
    auto result = restored.query_outstanding_memory(3);
    EXPECT_NE(result.error.find("64-bit"), std::string::npos);
    EXPECT_EQ(result.total.known.bytes, 0u);
    EXPECT_TRUE(result.functions.empty());
}
