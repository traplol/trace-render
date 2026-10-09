#include <gtest/gtest.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "parser/profile_io.h"
#include "parser/trace_parser.h"
#include "ui/memory_panel.h"
#include "ui/source_panel.h"
#include <limits>

namespace {
TraceModel memory_model() {
    TraceModel model;
    model.intern_string("");
    const char* ids[] = {"root", "allocate", "recursive", "other", "unresolved"};
    const char* names[] = {"main", "allocate", "allocate", "allocate", ""};
    const char* parents[] = {"", "root", "allocate", "root", "root"};
    const char* symbols[] = {"main", "allocate", "allocate", "different-symbol", ""};
    for (int i = 0; i < 5; ++i) {
        StackFrame frame;
        frame.id_idx = model.intern_string(ids[i]);
        frame.name_idx = model.intern_string(names[i]);
        if (*parents[i]) frame.parent_id = model.intern_string(parents[i]);
        if (*symbols[i]) frame.symbol_id = model.intern_string(symbols[i]);
        if (i == 4) frame.address = 0x1234;
        if (i == 1) {
            frame.source_file = model.intern_string("tests/fixtures/native_symbols/fixture.c");
            frame.source_line = 4;
        }
        model.add_stack_frame(frame);
    }
    ProfileData profile;
    profile.source_name = "removed.diagsession";
    profile.capture_start_ts = 0;
    profile.capture_end_ts = 100;
    profile.capabilities.native_allocation_history = true;
    profile.processes = {{"first", 7, std::nullopt, std::nullopt}, {"second", 7, std::nullopt, std::nullopt}};
    profile.heaps = {{"heap-first", "first", std::nullopt, std::nullopt},
                     {"heap-second", "second", std::nullopt, std::nullopt}};
    auto add = [&](const char* id, const char* stack, uint64_t bytes, std::optional<double> start,
                   std::optional<double> end, AllocationEnd state, const char* process = "first") {
        AllocationLifetime allocation;
        allocation.id = id;
        allocation.process_id = process;
        allocation.heap_id = std::string("heap-") + process;
        allocation.stack_frame_id = stack;
        allocation.size_bytes = bytes;
        allocation.allocated_ts = start;
        allocation.freed_ts = end;
        allocation.end_state = state;
        profile.allocations.push_back(allocation);
    };
    add("retained", "recursive", 100, 1, std::nullopt, AllocationEnd::LiveAtCaptureEnd);
    add("released", "allocate", 40, 20, 40, AllocationEnd::Freed);
    add("other-function", "other", 60, 30, std::nullopt, AllocationEnd::LiveAtCaptureEnd);
    add("unknown-birth", "other", 7, std::nullopt, 90, AllocationEnd::Freed);
    add("unknown-end", "unresolved", 5, 10, std::nullopt, AllocationEnd::Unknown);
    add("no-stack", "", 9, 5, std::nullopt, AllocationEnd::LiveAtCaptureEnd);
    add("other-process", "allocate", 20, 10, std::nullopt, AllocationEnd::LiveAtCaptureEnd, "second");
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}

TraceModel managed_memory_model(bool include_native = false) {
    auto model = memory_model();
    auto profile = model.profile();
    profile.capabilities.native_allocation_history = include_native;
    profile.capabilities.managed_allocation_history = true;
    profile.capabilities.managed_survival = true;
    if (!include_native) profile.allocations.clear();
    auto add = [&](const char* id, const char* stack, uint64_t bytes, double birth, const char* process) {
        AllocationLifetime allocation;
        allocation.id = id;
        allocation.process_id = process;
        allocation.heap_id = std::string("heap-") + process;
        allocation.stack_frame_id = stack;
        allocation.size_bytes = bytes;
        allocation.allocated_ts = birth;
        allocation.kind = AllocationKind::Managed;
        profile.allocations.push_back(allocation);
    };
    add("managed-retained", "recursive", 300, 10, "first");
    add("managed-collected", "allocate", 60, 20, "first");
    add("managed-other-process", "other", 30, 10, "second");
    profile.managed_survival = {{"managed-retained", 50, true, 0x200},
                                {"managed-collected", 40, true, 0x300},
                                {"managed-collected", 60, false, {}},
                                {"managed-other-process", 70, true, 0x400}};
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}

int32_t function_row(const MemoryPanel& panel, const std::string& process, int32_t frame) {
    for (size_t i = 0; i < panel.result().functions.size(); ++i) {
        const auto& row = panel.result().functions[i];
        if (row.process_id == process && row.frame_index == frame) return (int32_t)i;
    }
    return -1;
}

class MemoryPanelRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1800, 1400);
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override { ImGui::DestroyContext(); }

    template <typename Panel>
    std::string render(Panel& panel, const TraceModel& model, ViewState& view) {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        ImGui::SetNextWindowSize(ImVec2(1600, 1300));
        panel.render(model, view);
        std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::EndFrame();
        return text;
    }
};
}  // namespace

TEST(MemoryPanel, TimeBirthAndProcessControlsUseRecordedLifetimes) {
    auto model = memory_model();
    MemoryPanel panel;
    panel.refresh(model);
    EXPECT_EQ(panel.time(), 100);
    EXPECT_EQ(panel.result().total.known.bytes, 189u);
    EXPECT_EQ(panel.result().total.known.count, 4u);
    EXPECT_EQ(panel.result().total.uncertain.bytes, 5u);

    panel.set_time(30);
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 229u);
    EXPECT_EQ(panel.result().total.known.count, 5u);
    EXPECT_EQ(panel.result().total.uncertain.bytes, 12u);
    EXPECT_EQ(panel.result().total.uncertain.count, 2u);
    panel.set_birth_range({{20, 30}});
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 40u);
    EXPECT_EQ(panel.result().total.known.count, 1u);
    EXPECT_EQ(panel.result().total.uncertain.count, 0u);
    EXPECT_EQ(panel.result().unknown_birth_count, 1u);
    panel.set_time(40);
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.count, 0u);
    panel.set_birth_range(std::nullopt);
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 189u);
    EXPECT_EQ(panel.result().total.uncertain.bytes, 12u);

    panel.set_process("second");
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 20u);
    EXPECT_EQ(panel.result().total.known.count, 1u);
    EXPECT_EQ(panel.result().total.uncertain.count, 0u);
    panel.set_process({});
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 189u);
}

TEST(MemoryPanel, ManagedQueriesReuseFiltersAndPathsWithoutKeepingNativeSelections) {
    auto model = managed_memory_model(true);
    MemoryPanel panel;
    panel.set_time(40);
    panel.refresh(model);
    EXPECT_EQ(panel.allocation_kind(), AllocationKind::Native);
    EXPECT_EQ(panel.result().total.known.bytes, 189u);
    panel.select_function(model, function_row(panel, "first", 1));
    panel.select_stack(model, (int32_t)panel.contributing_stacks()[0]);

    panel.set_allocation_kind(AllocationKind::Managed);
    EXPECT_EQ(panel.selected_function(), -1);
    EXPECT_EQ(panel.selected_stack(), -1);
    EXPECT_TRUE(panel.selected_path().empty());
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 390u);
    panel.select_function(model, function_row(panel, "first", 1));
    ASSERT_EQ(panel.contributing_stacks().size(), 2u);
    panel.select_stack(model, (int32_t)panel.contributing_stacks()[0]);
    EXPECT_EQ(panel.selected_path(), (std::vector<int32_t>{0, 1, 2}));

    panel.set_birth_range({{20, 30}});
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 60u);
    EXPECT_EQ(panel.selected_stack(), -1);
    panel.set_time(50);
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 0u);
    EXPECT_EQ(panel.result().total.uncertain.bytes, 60u);
    panel.set_time(60);
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.count, 0u);
    EXPECT_EQ(panel.result().total.uncertain.count, 0u);

    panel.set_birth_range(std::nullopt);
    panel.set_process("second");
    panel.refresh(model);
    EXPECT_EQ(panel.result().total.known.bytes, 30u);
    EXPECT_EQ(panel.result().total.uncertain.count, 0u);
    panel.set_allocation_kind(AllocationKind::Native);
    panel.refresh(model);
    EXPECT_EQ(panel.time(), 60);
    EXPECT_EQ(panel.process_id(), "second");
    EXPECT_EQ(panel.result().total.known.bytes, 20u);

    panel.set_allocation_kind(AllocationKind::Managed);
    panel.on_model_changed();
    panel.refresh(model);
    EXPECT_EQ(panel.allocation_kind(), AllocationKind::Native);
    EXPECT_EQ(panel.time(), 100);
    EXPECT_TRUE(panel.process_id().empty());
    EXPECT_EQ(panel.result().total.known.bytes, 189u);
}

TEST_F(MemoryPanelRenderTest, ManagedAllocationsDefaultToTheirViewAndExplainCheckpointUncertainty) {
    auto model = managed_memory_model();
    auto profile = model.profile();
    profile.capabilities.managed_heap_snapshots = true;
    profile.managed_snapshots.push_back({"snapshot", "first", 50, 48, 2});
    model.set_profile(std::move(profile));
    model.build_index();
    MemoryPanel panel;
    ViewState view;
    panel.set_time(50);
    auto text = render(panel, model, view);
    EXPECT_EQ(panel.allocation_kind(), AllocationKind::Managed);
    EXPECT_NE(text.find("Managed outstanding memory"), std::string::npos) << text;
    EXPECT_NE(text.find("Known outstanding at 50.000 us: 330 bytes in 2 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("Uncertain records: 60 bytes in 1 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("GC checkpoints bound observed survival and absence"), std::string::npos) << text;
    EXPECT_NE(text.find("Unobserved intervals remain uncertain"), std::string::npos) << text;
    EXPECT_NE(text.find("does not give the exact release time"), std::string::npos) << text;
    EXPECT_EQ(text.find("Native outstanding memory"), std::string::npos) << text;

    profile = model.profile();
    profile.capabilities.managed_survival = false;
    profile.quality.sampled_allocations = true;
    model.set_profile(std::move(profile));
    model.build_index();
    panel.on_model_changed();
    panel.set_time(50);
    text = render(panel, model, view);
    EXPECT_NE(text.find("GC survival observations are unavailable"), std::string::npos) << text;
    EXPECT_NE(text.find("Values describe recorded samples"), std::string::npos) << text;
    EXPECT_NE(text.find("Uncertain records: 390 bytes in 3 allocations"), std::string::npos) << text;
}

TEST_F(MemoryPanelRenderTest, MixedProfilesExposeAllMemoryViewsAndResetTheDefaultOnReplacement) {
    auto model = managed_memory_model(true);
    auto profile = model.profile();
    profile.capabilities.managed_heap_snapshots = true;
    ManagedSnapshot snapshot;
    snapshot.id = "snapshot";
    snapshot.process_id = "first";
    snapshot.ts = 50;
    snapshot.live_bytes = 48;
    snapshot.object_count = 2;
    profile.managed_snapshots.push_back(snapshot);
    model.set_profile(std::move(profile));
    model.build_index();
    MemoryPanel panel;
    ViewState view;
    auto text = render(panel, model, view);
    for (const char* choice : {"Native outstanding memory", "Managed outstanding memory", "Managed snapshots"})
        EXPECT_NE(text.find(choice), std::string::npos) << text;
    EXPECT_NE(text.find("Known outstanding at 100.000 us: 189 bytes in 4 allocations"), std::string::npos) << text;
    EXPECT_EQ(text.find("GC checkpoints"), std::string::npos) << text;

    panel.set_allocation_kind(AllocationKind::Managed);
    panel.set_time(50);
    text = render(panel, model, view);
    EXPECT_NE(text.find("Known outstanding at 50.000 us: 330 bytes in 2 allocations"), std::string::npos) << text;
    panel.select_snapshot(0);
    text = render(panel, model, view);
    EXPECT_NE(text.find("Recorded bytes: 48"), std::string::npos) << text;
    EXPECT_EQ(text.find("Known outstanding"), std::string::npos) << text;
    panel.set_allocation_kind(AllocationKind::Managed);
    text = render(panel, model, view);
    EXPECT_NE(text.find("Known outstanding at 50.000 us: 330 bytes in 2 allocations"), std::string::npos) << text;

    panel.select_snapshot(0);
    panel.on_model_changed();
    text = render(panel, model, view);
    EXPECT_NE(text.find("Known outstanding at 100.000 us: 189 bytes in 4 allocations"), std::string::npos) << text;
    EXPECT_EQ(text.find("Recorded bytes"), std::string::npos) << text;
}

TEST_F(MemoryPanelRenderTest, ManagedSnapshotsShowRecordedCountsWithoutAllocationAttribution) {
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    profile.capabilities.managed_heap_snapshots = true;
    profile.processes = {{"managed-process", 7, {}, {}}};
    ManagedSnapshot first;
    first.id = "first";
    first.process_id = "managed-process";
    first.ts = 100;
    first.live_bytes = 48;
    first.object_count = 2;
    first.types = {{"retained", "RetainedPayload", 2, 48, 4}};
    first.sampled = first.incomplete = true;
    first.average_count_multiplier = 2;
    first.average_size_multiplier = 3;
    first.warnings = {"Recorded snapshot coverage note"};
    auto second = first;
    second.id = "second";
    second.ts = 200;
    second.live_bytes = 96;
    second.object_count = 4;
    second.types[0].object_count = 4;
    second.types[0].size_bytes = 96;
    profile.managed_snapshots = {first, second};
    model.set_profile(std::move(profile));
    model.build_index();
    MemoryPanel panel;
    ViewState view;
    auto text = render(panel, model, view);
    EXPECT_NE(text.find("Allocation attribution unavailable"), std::string::npos);
    EXPECT_NE(text.find("Recorded bytes: 48"), std::string::npos);
    EXPECT_NE(text.find("Recorded objects: 2"), std::string::npos);
    EXPECT_NE(text.find("Sampled snapshot"), std::string::npos);
    EXPECT_NE(text.find("Partial snapshot"), std::string::npos);
    EXPECT_NE(text.find("Recorded snapshot coverage note"), std::string::npos);
    EXPECT_EQ(text.find("Functions on allocation stacks"), std::string::npos);
    panel.select_snapshot(1);
    text = render(panel, model, view);
    EXPECT_NE(text.find("Recorded bytes: 96"), std::string::npos);
    EXPECT_NE(text.find("Recorded objects: 4"), std::string::npos);
    panel.on_model_changed();
    EXPECT_EQ(panel.selected_snapshot(), 0u);
}

TEST(MemoryPanel, FunctionSelectionKeepsDistinctIdentitiesAndRecursivePaths) {
    auto model = memory_model();
    MemoryPanel panel;
    panel.set_time(30);
    panel.refresh(model);
    int32_t allocating = function_row(panel, "first", 1);
    ASSERT_GE(allocating, 0);
    EXPECT_EQ(panel.result().functions[allocating].inclusive.known.bytes, 140u);
    EXPECT_EQ(panel.result().functions[allocating].inclusive.known.count, 2u);
    panel.select_function(model, allocating);
    ASSERT_EQ(panel.contributing_stacks().size(), 2u);
    auto retained = panel.contributing_stacks()[0];
    EXPECT_EQ(panel.result().stacks[retained].frame_index, 2);
    panel.select_stack(model, (int32_t)retained);
    EXPECT_EQ(panel.selected_path(), (std::vector<int32_t>{0, 1, 2}));
    panel.set_time(40);
    panel.refresh(model);
    ASSERT_EQ(panel.contributing_stacks().size(), 1u);
    EXPECT_EQ(panel.selected_path(), (std::vector<int32_t>{0, 1, 2}));
    EXPECT_EQ(panel.result().functions[panel.selected_function()].inclusive.known.bytes, 100u);

    panel.select_function(model, function_row(panel, "first", 3));
    ASSERT_EQ(panel.contributing_stacks().size(), 1u);
    EXPECT_EQ(panel.result().stacks[panel.contributing_stacks()[0]].frame_index, 3);
    EXPECT_TRUE(panel.selected_path().empty());
    panel.select_stack(model, (int32_t)retained);
    EXPECT_EQ(panel.selected_stack(), -1);  // A path from the previously selected function is excluded.

    panel.select_function(model, function_row(panel, "first", -1));
    ASSERT_EQ(panel.contributing_stacks().size(), 1u);
    panel.select_stack(model, (int32_t)panel.contributing_stacks()[0]);
    EXPECT_EQ(panel.result().stacks[panel.selected_stack()].frame_index, -1);
    EXPECT_TRUE(panel.selected_path().empty());
    EXPECT_FALSE(model.memory_stack_contains_function(-1, 0));
    EXPECT_FALSE(model.memory_stack_contains_function(2, -1));
    EXPECT_FALSE(model.memory_stack_contains_function(100, 1));
    EXPECT_FALSE(model.memory_stack_contains_function(2, 100));

    panel.set_birth_range({{80, 90}});
    panel.refresh(model);
    EXPECT_EQ(panel.selected_function(), -1);
    EXPECT_EQ(panel.selected_stack(), -1);
    EXPECT_TRUE(panel.contributing_stacks().empty());
}

TEST(MemoryPanel, ReplacingModelClearsFiltersSelectionsAndCachedQuery) {
    auto model = memory_model();
    MemoryPanel panel;
    panel.set_time(30);
    panel.set_process("first");
    panel.set_birth_range({{0, 30}});
    panel.refresh(model);
    panel.select_function(model, function_row(panel, "first", 1));
    panel.select_stack(model, (int32_t)panel.contributing_stacks()[0]);
    auto replacement = model.profile();
    replacement.capture_end_ts = 80;
    replacement.allocations[0].size_bytes = 500;
    model.set_profile(std::move(replacement));
    model.build_index();
    panel.on_model_changed();
    panel.refresh(model);
    EXPECT_EQ(panel.time(), 80);
    EXPECT_TRUE(panel.process_id().empty());
    EXPECT_FALSE(panel.birth_range());
    EXPECT_EQ(panel.selected_function(), -1);
    EXPECT_EQ(panel.selected_stack(), -1);
    EXPECT_TRUE(panel.selected_path().empty());
    EXPECT_EQ(panel.result().total.known.bytes, 589u);
    panel.set_time(std::numeric_limits<double>::quiet_NaN());
    panel.refresh(model);
    EXPECT_FALSE(panel.result().error.empty());
    EXPECT_TRUE(panel.result().functions.empty());
    EXPECT_EQ(panel.result().total.known.count, 0u);
}

TEST_F(MemoryPanelRenderTest, ShowsOutstandingMetricsUncertaintyAndUnresolvedOrigins) {
    auto model = memory_model();
    ViewState view;
    MemoryPanel panel;
    panel.set_time(30);
    auto text = render(panel, model, view);
    EXPECT_NE(text.find("Known outstanding at 30.000 us: 229 bytes in 5 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("Uncertain records: 12 bytes in 2 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("Partial capture"), std::string::npos) << text;
    EXPECT_NE(text.find("not process RAM, total allocated bytes, or CPU time"), std::string::npos) << text;
    EXPECT_NE(text.find("Inclusive rows overlap"), std::string::npos) << text;
    EXPECT_NE(text.find("0x1234"), std::string::npos) << text;
    EXPECT_NE(text.find("Unknown allocation stack"), std::string::npos) << text;
    panel.select_function(model, function_row(panel, "first", 1));
    panel.select_stack(model, (int32_t)panel.contributing_stacks()[0]);
    text = render(panel, model, view);
    EXPECT_NE(text.find("main > allocate > allocate"), std::string::npos) << text;
    EXPECT_NE(text.find("View source"), std::string::npos) << text;
    EXPECT_NE(text.find("Source unavailable"), std::string::npos) << text;
    panel.set_birth_range({{20, 30}});
    text = render(panel, model, view);
    EXPECT_NE(text.find("Birth filter omitted 1 allocations"), std::string::npos) << text;

    auto sampled = model.profile();
    sampled.quality.sampled_allocations = true;
    sampled.quality.warnings = {"Provider reported lost allocation events."};
    model.set_profile(std::move(sampled));
    model.build_index();
    panel.on_model_changed();
    panel.set_time(30);
    text = render(panel, model, view);
    EXPECT_NE(text.find("0 bytes in 0 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("Uncertain records: 241 bytes in 7 allocations"), std::string::npos) << text;
    EXPECT_NE(text.find("Values describe recorded samples"), std::string::npos) << text;
    EXPECT_NE(text.find("Provider reported lost allocation events."), std::string::npos) << text;
}

TEST_F(MemoryPanelRenderTest, SnapshotOnlyProfilesExplainMissingAllocationOrigins) {
    TraceModel model;
    ProfileData profile;
    profile.capabilities.managed_heap_snapshots = true;
    model.set_profile(std::move(profile));
    model.build_index();
    ViewState view;
    MemoryPanel panel;
    auto text = render(panel, model, view);
    EXPECT_NE(text.find("Allocation history is unavailable"), std::string::npos) << text;
    EXPECT_NE(text.find("Heap snapshots alone do not record allocation origins"), std::string::npos) << text;
    EXPECT_EQ(text.find("Known outstanding"), std::string::npos) << text;
}

TEST_F(MemoryPanelRenderTest, SavedMemoryFramesNavigateSourceWithoutCpuEvents) {
    auto original = memory_model();
    std::string data, error;
    ASSERT_TRUE(serialize_profile(original, data, error)) << error;
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), model)) << parser.error_message();
    ASSERT_TRUE(model.events().empty());
    MemoryPanel memory;
    memory.refresh(model);
    memory.select_function(model, function_row(memory, "first", 1));
    ASSERT_FALSE(memory.contributing_stacks().empty());
    memory.select_stack(model, (int32_t)memory.contributing_stacks()[0]);
    ASSERT_EQ(memory.selected_path(), (std::vector<int32_t>{0, 1, 2}));
    ViewState view;
    view.set_pending_scroll_event_idx(7);
    view.select_stack_frame(memory.selected_path()[1]);
    EXPECT_EQ(view.selected_event_idx(), -1);
    EXPECT_EQ(view.pending_scroll_event_idx(), -1);
    SourcePanel source;
    auto text = render(source, model, view);
    EXPECT_NE(text.find("tests/fixtures/native_symbols/fixture.c"), std::string::npos) << text;
    EXPECT_EQ(text.find("Could not open"), std::string::npos) << text;
    EXPECT_EQ(text.find("Select an event"), std::string::npos) << text;

    // Reusing the same frame index in another model must not keep the old file cached.
    model.set_stack_frame_symbol(1, "allocate", "allocate", "missing-source.cpp", 8, true);
    source.on_model_changed();
    text = render(source, model, view);
    EXPECT_NE(text.find("Could not open: missing-source.cpp"), std::string::npos) << text;
    EXPECT_EQ(text.find("fixture.c"), std::string::npos) << text;
    view.set_selected_event_idx(0);
    EXPECT_EQ(view.selected_stack_frame_idx(), -1);
    view.select_stack_frame(1);
    view.navigate_to_event(2, TraceEvent{});
    EXPECT_EQ(view.selected_event_idx(), 2);
    EXPECT_EQ(view.selected_stack_frame_idx(), -1);
}
