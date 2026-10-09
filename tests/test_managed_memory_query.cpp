#include <gtest/gtest.h>
#include "model/trace_model.h"
#include "parser/profile_io.h"
#include <cmath>
#include <limits>

namespace {
ProfileData capture() {
    ProfileData profile;
    profile.capture_start_ts = 0;
    profile.capture_end_ts = 100;
    profile.capabilities.managed_allocation_history = true;
    profile.capabilities.managed_survival = true;
    profile.processes = {{"process", 7, 0, std::nullopt}};
    profile.heaps = {{"heap", "process", 0, std::nullopt}};
    return profile;
}
AllocationLifetime allocation(const std::string& id, std::optional<double> birth, uint64_t bytes,
                              const std::string& stack = "leaf") {
    AllocationLifetime result;
    result.id = id;
    result.process_id = "process";
    result.heap_id = "heap";
    result.address = 0x100;
    result.size_bytes = bytes;
    result.stack_frame_id = stack;
    result.kind = AllocationKind::Managed;
    result.allocated_ts = birth;
    return result;
}
TraceModel model_for(ProfileData profile) {
    TraceModel model;
    model.intern_string("");
    auto frame = [&](const std::string& id, const std::string& parent, const std::string& symbol) {
        StackFrame value;
        value.id_idx = model.intern_string(id);
        value.name_idx = model.intern_string("same display name");
        value.symbol_id = model.intern_string(symbol);
        if (!parent.empty()) value.parent_id = model.intern_string(parent);
        model.add_stack_frame(value);
    };
    frame("root", "", "main");
    frame("leaf", "root", "allocate");
    frame("recursive", "leaf", "allocate");
    frame("other", "root", "another-allocator");
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}
OutstandingMemory managed(const TraceModel& model, double ts,
                          std::optional<std::pair<double, double>> births = std::nullopt,
                          const std::string& process = {}) {
    return model.query_outstanding_memory(ts, births, process, AllocationKind::Managed);
}
void expect_amount(const OutstandingMemory& result, uint64_t known, uint64_t uncertain, uint64_t known_count,
                   uint64_t uncertain_count) {
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.total.known.bytes, known);
    EXPECT_EQ(result.total.uncertain.bytes, uncertain);
    EXPECT_EQ(result.total.known.count, known_count);
    EXPECT_EQ(result.total.uncertain.count, uncertain_count);
}
}  // namespace

TEST(ManagedMemoryQuery, PositiveCheckpointsAreInclusiveAndNegativeCheckpointsDoNotInventFreeTimes) {
    auto profile = capture();
    profile.allocations.push_back(allocation("object", 10, 40));
    profile.allocations[0].end_state = AllocationEnd::Freed;  // Observed by GC, exact release remains unknown.
    profile.managed_survival = {
        {"object", 70, false, {}}, {"object", 20, true, 0x200}, {"object", 80, false, {}}, {"object", 50, true, 0x300}};
    auto model = model_for(profile);
    expect_amount(managed(model, 9), 0, 0, 0, 0);
    for (double ts : {10, 30, 50}) expect_amount(managed(model, ts), 40, 0, 1, 0);
    for (double ts : {std::nextafter(50.0, 100.0), 60.0, std::nextafter(70.0, 0.0)}) {
        auto result = managed(model, ts);
        expect_amount(result, 0, 40, 0, 1);
        EXPECT_TRUE(result.incomplete);
    }
    for (double ts : {70, 80, 100}) expect_amount(managed(model, ts), 0, 0, 0, 0);
    EXPECT_FALSE(model.profile().allocations[0].freed_ts);
    EXPECT_EQ(model.profile().allocations[0].address, 0x100u);
    EXPECT_EQ(model.profile().managed_survival[3].address, 0x300u);
}

TEST(ManagedMemoryQuery, BirthIsAnObservationButAnUnobservedTailRemainsUncertain) {
    auto profile = capture();
    profile.allocations.push_back(allocation("object", 10, 20));
    auto model = model_for(profile);
    expect_amount(managed(model, 10), 20, 0, 1, 0);
    expect_amount(managed(model, std::nextafter(10.0, 100.0)), 0, 20, 0, 1);
    expect_amount(managed(model, 100), 0, 20, 0, 1);
    profile.allocations[0].end_state = AllocationEnd::LiveAtCaptureEnd;
    expect_amount(managed(model_for(profile), 100), 20, 0, 1, 0);
    profile.allocations[0].end_state = AllocationEnd::Unknown;
    profile.managed_survival = {{"object", 50, true, {}}, {"object", 70, false, {}}};
    profile.capabilities.managed_survival = false;
    model = model_for(profile);
    expect_amount(managed(model, 30), 0, 20, 0, 1);
    expect_amount(managed(model, 80), 0, 20, 0, 1);
    EXPECT_FALSE(model.query_outstanding_memory(30).error.empty());
    profile.capabilities.managed_allocation_history = false;
    model = model_for(profile);
    EXPECT_EQ(managed(model, 30).error, "Managed allocation history is unavailable.");
}

TEST(ManagedMemoryQuery, MovedAndReusedAddressesKeepOriginsCohortsAndAllocationKindsSeparate) {
    auto profile = capture();
    profile.capabilities.native_allocation_history = true;
    profile.processes.push_back({"other-process", 8, 0, std::nullopt});
    profile.allocations = {allocation("moved", 10, 10, "recursive"), allocation("reused", 25, 20, "other"),
                           allocation("other-process-object", 30, 30), allocation("unknown-birth", {}, 5, ""),
                           allocation("native", 5, 50)};
    profile.allocations[2].process_id = "other-process";
    profile.allocations[2].heap_id.clear();
    profile.allocations[4].kind = AllocationKind::Native;
    profile.allocations[4].end_state = AllocationEnd::LiveAtCaptureEnd;
    profile.managed_survival = {{"moved", 20, true, 0x200},
                                {"moved", 40, true, 0x300},
                                {"moved", 50, false, {}},
                                {"reused", 35, true, 0x100},
                                {"other-process-object", 45, true, 0x100},
                                {"unknown-birth", 35, true, 0x400}};
    auto model = model_for(profile);
    expect_amount(model.query_outstanding_memory(35), 50, 0, 1, 0);
    auto at_checkpoint = managed(model, 35);
    expect_amount(at_checkpoint, 60, 5, 3, 1);
    expect_amount(managed(model, 45), 30, 35, 1, 3);
    expect_amount(managed(model, 35, {}, "process"), 30, 5, 2, 1);
    auto cohort = managed(model, 35, {{25, 30}});
    expect_amount(cohort, 20, 0, 1, 0);
    EXPECT_EQ(cohort.unknown_birth_count, 1u);
    expect_amount(managed(model, 35, {{10, 25}}), 10, 0, 1, 0);
    expect_amount(managed(model, 35, {{25, 25}}), 0, 0, 0, 0);
    EXPECT_FALSE(managed(model, 35, {{30, 25}}).error.empty());
    EXPECT_FALSE(managed(model, std::numeric_limits<double>::quiet_NaN()).error.empty());

    size_t allocator_groups = 0, unattributed = 0;
    for (const auto& function : at_checkpoint.functions) {
        if (function.frame_index == -1) {
            ++unattributed;
            EXPECT_EQ(function.exclusive.uncertain.bytes, 5u);
            continue;
        }
        const auto& frame = model.stack_frames()[function.frame_index];
        if (model.get_string(frame.symbol_id) != "allocate") continue;
        ++allocator_groups;
        const auto expected = function.process_id == "process" ? 10u : 30u;
        EXPECT_EQ(function.inclusive.known.bytes, expected);
        EXPECT_EQ(function.exclusive.known.bytes, expected);
        EXPECT_EQ(function.inclusive.known.count, 1u);  // Recursion does not multiply an allocation.
    }
    EXPECT_EQ(allocator_groups, 2u);
    EXPECT_EQ(unattributed, 1u);
}

TEST(ManagedMemoryQuery, ExactEndsHeapProcessAndCaptureBoundsStillApply) {
    auto profile = capture();
    profile.processes[0].end_ts = 80;
    profile.heaps[0].end_ts = 70;
    profile.allocations = {allocation("exact", 10, 10), allocation("live", 10, 20), allocation("unknown", 10, 30)};
    profile.allocations[0].freed_ts = 40;
    profile.allocations[0].end_state = AllocationEnd::Freed;
    profile.allocations[2].heap_id.clear();
    profile.managed_survival = {
        {"exact", 20, true, {}}, {"exact", 60, false, {}}, {"live", 60, true, {}}, {"unknown", 50, true, {}}};
    auto model = model_for(profile);
    expect_amount(managed(model, 35), 60, 0, 3, 0);
    expect_amount(managed(model, 40), 50, 0, 2, 0);
    expect_amount(managed(model, 60), 20, 30, 1, 1);
    expect_amount(managed(model, 70), 0, 30, 0, 1);
    expect_amount(managed(model, 80), 0, 0, 0, 0);
    EXPECT_FALSE(managed(model, -1).error.empty());
    EXPECT_FALSE(managed(model, 101).error.empty());
}

TEST(ManagedMemoryQuery, ExistingCoverageFlagsStayConservativeAndOverflowClearsTotals) {
    auto profile = capture();
    profile.allocations.push_back(allocation("object", 10, 20));
    profile.managed_survival.push_back({"object", 50, true, {}});
    for (int mode = 0; mode < 3; ++mode) {
        auto limited = profile;
        limited.quality.lost_events = mode == 0 ? 1 : 0;
        limited.quality.allocation_history_gaps = mode == 1;
        limited.quality.sampled_allocations = mode == 2;
        auto result = managed(model_for(limited), 20);
        expect_amount(result, 0, 20, 0, 1);
        EXPECT_TRUE(result.incomplete);
    }
    profile.quality.incomplete_capture = true;  // A partial initial heap need not invalidate recorded survival.
    auto result = managed(model_for(profile), 20);
    expect_amount(result, 20, 0, 1, 0);
    EXPECT_TRUE(result.incomplete);
    profile.allocations[0].size_bytes = UINT64_MAX;
    profile.allocations.push_back(allocation("uncertain", 10, 1));
    result = managed(model_for(profile), 20);
    EXPECT_NE(result.error.find("64-bit"), std::string::npos);
    EXPECT_EQ(result.total.known.bytes, 0u);
    EXPECT_EQ(result.total.uncertain.bytes, 0u);
    EXPECT_TRUE(result.stacks.empty());
    EXPECT_TRUE(result.functions.empty());
}

TEST(ManagedMemoryQuery, SavedProfilesKeepCheckpointFactsAndRebuildTheSameQuery) {
    auto profile = capture();
    profile.allocations = {allocation("first", 10, 10), allocation("second", 30, 20)};
    profile.managed_survival = {{"first", 40, true, 0x200}, {"first", 60, false, {}}, {"second", 50, true, 0x100}};
    auto original = model_for(profile);
    std::string saved, error;
    ASSERT_TRUE(serialize_profile(original, saved, error)) << error;
    TraceModel reopened;
    ASSERT_TRUE(read_profile(saved, reopened, error)) << error;
    ASSERT_EQ(reopened.profile().managed_survival.size(), 3u);
    EXPECT_EQ(reopened.profile().managed_survival[0].address, 0x200u);
    EXPECT_FALSE(reopened.profile().managed_survival[1].survived);
    EXPECT_EQ(reopened.profile().managed_survival[1].ts, 60);
    EXPECT_FALSE(reopened.profile().allocations[0].freed_ts);
    for (double ts : {10, 30, 40, 45, 50, 60, 100}) {
        auto before = managed(original, ts, {{10, 40}}), after = managed(reopened, ts, {{10, 40}});
        expect_amount(after, before.total.known.bytes, before.total.uncertain.bytes, before.total.known.count,
                      before.total.uncertain.count);
        EXPECT_EQ(after.incomplete, before.incomplete);
        ASSERT_EQ(after.functions.size(), before.functions.size());
        for (size_t i = 0; i < before.functions.size(); ++i) {
            EXPECT_EQ(after.functions[i].inclusive.known.bytes, before.functions[i].inclusive.known.bytes);
            EXPECT_EQ(after.functions[i].inclusive.uncertain.bytes, before.functions[i].inclusive.uncertain.bytes);
        }
    }
}
