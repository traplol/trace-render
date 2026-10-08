#include <gtest/gtest.h>
#include "parser/profile_io.h"
#include "parser/trace_parser.h"
#include "platform/file_loader.h"
#include "model/query_db.h"
#include "ui/flame_graph_panel.h"
#include "ui/source_panel.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace {
TraceModel representative_profile() {
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    profile.source_format = "diagsession";
    profile.source_name = "/unavailable/capture.diagsession";
    profile.converter = "test-capture-decoder/1";
    profile.capture_start_ts = 0;
    profile.capture_end_ts = 100;
    profile.capabilities = {true, true, true, true, true, true};
    profile.quality = {true, true, true, true, 12, {"Allocation coverage is incomplete between 20 and 30 us"}};
    profile.processes = {{"9007199254740993", 42, std::nullopt, 50}, {"9007199254740994", 42, 60, std::nullopt}};
    profile.heaps = {{"heap-first", "9007199254740993", std::nullopt, 50},
                     {"heap-reused", "9007199254740994", 60, std::nullopt}};
    profile.modules = {{"module-first", "9007199254740993", "app.exe", "/unavailable/app.exe", "pdb-guid-age-1",
                        0x140000000, 4096, std::nullopt, 50},
                       {"module-reused", "9007199254740994", "app.exe", "/unavailable/app.exe", "pdb-guid-age-2",
                        0x140000000, 4096, 60, std::nullopt}};

    StackFrame root;
    root.id_idx = model.intern_string("root");
    root.name_idx = model.intern_string("main");
    root.module_id = model.intern_string("module-first");
    root.symbol_id = model.intern_string("module-first:main");
    root.address = 0x140000010;
    root.symbol_resolved = true;
    model.add_stack_frame(root);
    StackFrame leaf;
    leaf.id_idx = model.intern_string("leaf");
    leaf.parent_id = root.id_idx;
    leaf.name_idx = model.intern_string("same_name");
    leaf.module_id = root.module_id;
    leaf.symbol_id = model.intern_string("module-first:same_name");
    leaf.address = UINT64_C(9007199254740993);
    leaf.source_file = model.intern_string("C:\\sources\\app.cpp");
    leaf.source_line = 37;
    leaf.symbol_resolved = true;
    model.add_stack_frame(leaf);
    StackFrame other = leaf;
    other.id_idx = model.intern_string("same-name-other-module");
    other.parent_id = UINT32_MAX;
    other.module_id = model.intern_string("module-reused");
    other.symbol_id = model.intern_string("module-reused:same_name");
    other.address = UINT64_MAX;
    other.symbol_resolved = false;
    other.source_file = 0;
    other.source_line = 0;
    model.add_stack_frame(other);

    TraceEvent measured;
    measured.name_idx = model.intern_string("measured operation");
    measured.ph = Phase::Complete;
    measured.ts = 1.125;
    measured.dur = 2.5;
    measured.pid = 42;
    measured.tid = 11;
    measured.id = UINT64_MAX;
    measured.process_instance_id = model.intern_string("9007199254740993");
    measured.args_idx = model.add_args(R"({"file":"C:\\sources\\caller.cpp","line":4})");
    model.add_event(measured);
    TraceEvent sample;
    sample.ph = Phase::Sample;
    sample.kind = EventKind::Sample;
    sample.ts = 1.23456789012345;
    sample.pid = 42;
    sample.tid = 11;
    sample.process_instance_id = measured.process_instance_id;
    sample.stack_frame_id = leaf.id_idx;
    sample.sample_weight = 125;
    sample.sample_weight_unit = model.intern_string("ns");
    sample.sample_cpu_time = 0.125;
    model.add_event(sample);
    sample.ts = 10.75;
    sample.sample_weight = -1;
    sample.sample_weight_unit = 0;
    sample.sample_cpu_time = -1;
    model.add_event(sample);
    sample.ts = 65;
    sample.stack_frame_id = other.id_idx;
    sample.process_instance_id = model.intern_string("9007199254740994");
    sample.sample_weight = 7;
    sample.sample_weight_unit = model.intern_string("ticks");
    model.add_event(sample);
    TraceEvent span = measured;
    span.kind = EventKind::SampledSpan;
    span.ts = 12;
    span.dur = 5;
    model.add_event(span);
    auto& process = model.get_or_create_process(42);
    process.name = "demo process";
    process.get_or_create_thread(11).name = "worker";
    auto& counter = model.find_or_create_counter_series(42, "working set");
    counter.unit = "bytes";
    counter.points = {{0, 1000}, {100, 4000}};

    profile.allocations = {{"18446744073709551615", "9007199254740993", "heap-first", UINT64_C(9007199254740993),
                            UINT64_C(9007199254740993), "leaf", "", AllocationKind::Native, 1, 3, AllocationEnd::Freed},
                           {"reuse-same-heap", "9007199254740993", "heap-first", UINT64_C(9007199254740993), 256,
                            "leaf", "", AllocationKind::Native, 4, std::nullopt, AllocationEnd::Unknown},
                           {"reuse-process-and-heap", "9007199254740994", "heap-reused", UINT64_C(9007199254740993),
                            512, "same-name-other-module", "", AllocationKind::Native, std::nullopt, std::nullopt,
                            AllocationEnd::LiveAtCaptureEnd},
                           {"managed-object", "9007199254740993", "", 1234, 32, "", "type-1", AllocationKind::Managed,
                            std::nullopt, std::nullopt, AllocationEnd::Freed}};
    profile.managed_snapshots = {{"snapshot-1",
                                  "9007199254740993",
                                  8,
                                  UINT64_C(9007199254740993),
                                  UINT64_MAX,
                                  {{"type-1", "Demo.Buffer", UINT64_MAX, UINT64_C(9007199254740993)}}},
                                 {"snapshot-2", "9007199254740993", 16, std::nullopt, std::nullopt, {}}};
    profile.managed_survival = {{"managed-object", 8, true, 5678}, {"managed-object", 16, false, std::nullopt}};
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}

std::string encode(const TraceModel& model) {
    std::string data, error;
    EXPECT_TRUE(serialize_profile(model, data, error)) << error;
    return data;
}
}  // namespace

TEST(ProfileIo, RoundTripsCpuLifetimesAndManagedObservations) {
    auto original = representative_profile();
    auto data = encode(original);
    TraceParser parser;
    parser.set_time_unit_ns(true);  // Chrome's override must not alter explicit profile units.
    TraceModel restored;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), restored)) << parser.error_message();
    ASSERT_EQ(restored.events().size(), 5u);
    EXPECT_EQ(restored.events()[0].id, UINT64_MAX);
    EXPECT_EQ(restored.events()[0].kind, EventKind::Measured);
    EXPECT_DOUBLE_EQ(restored.events()[0].ts, 1.125);
    EXPECT_DOUBLE_EQ(restored.events()[0].dur, 2.5);
    EXPECT_DOUBLE_EQ(restored.events()[1].ts, original.events()[1].ts);
    EXPECT_DOUBLE_EQ(restored.events()[1].sample_weight, 125);
    EXPECT_EQ(restored.get_string(restored.events()[1].sample_weight_unit), "ns");
    EXPECT_DOUBLE_EQ(restored.events()[1].sample_cpu_time, 0.125);
    EXPECT_LT(restored.events()[2].sample_weight, 0);
    EXPECT_LT(restored.events()[2].sample_cpu_time, 0);
    EXPECT_EQ(restored.get_string(restored.events()[3].sample_weight_unit), "ticks");
    EXPECT_LT(restored.events()[3].sample_cpu_time, 0);
    EXPECT_EQ(restored.events()[4].kind, EventKind::SampledSpan);
    EXPECT_EQ(restored.build_sample_stack(1), (std::vector<uint32_t>{0, 1}));
    ASSERT_EQ(restored.stack_frames().size(), 3u);
    EXPECT_EQ(restored.stack_frames()[1].name_idx, restored.stack_frames()[2].name_idx);
    EXPECT_NE(restored.stack_frames()[1].module_id, restored.stack_frames()[2].module_id);
    EXPECT_NE(restored.stack_frames()[1].symbol_id, restored.stack_frames()[2].symbol_id);
    EXPECT_EQ(restored.stack_frames()[2].address, UINT64_MAX);
    EXPECT_EQ(restored.find_process(42)->name, "demo process");
    EXPECT_EQ(restored.find_thread(42, 11)->name, "worker");
    ASSERT_EQ(restored.counter_series().size(), 1u);
    EXPECT_EQ(restored.counter_series()[0].unit, "bytes");
    EXPECT_EQ(restored.counter_series()[0].points, original.counter_series()[0].points);

    const auto& p = restored.profile();
    ASSERT_EQ(p.allocations.size(), 4u);
    EXPECT_EQ(p.allocations[0].id, "18446744073709551615");
    EXPECT_EQ(p.allocations[0].size_bytes, UINT64_C(9007199254740993));
    EXPECT_EQ(p.allocations[0].address, p.allocations[1].address);
    EXPECT_NE(p.allocations[0].id, p.allocations[1].id);
    EXPECT_EQ(p.allocations[0].freed_ts, 3);
    EXPECT_FALSE(p.allocations[1].freed_ts);
    EXPECT_EQ(p.allocations[1].end_state, AllocationEnd::Unknown);
    EXPECT_FALSE(p.allocations[2].allocated_ts);
    EXPECT_EQ(p.allocations[2].end_state, AllocationEnd::LiveAtCaptureEnd);
    EXPECT_NE(p.allocations[1].process_id, p.allocations[2].process_id);
    EXPECT_NE(p.allocations[1].heap_id, p.allocations[2].heap_id);
    EXPECT_EQ(p.allocations[3].end_state, AllocationEnd::Freed);
    EXPECT_FALSE(p.allocations[3].freed_ts);  // GC death observation does not invent an exact free time.
    ASSERT_EQ(p.managed_snapshots.size(), 2u);
    EXPECT_EQ(p.managed_snapshots[0].types[0].object_count, UINT64_MAX);
    EXPECT_EQ(p.managed_snapshots[0].live_bytes, UINT64_C(9007199254740993));
    EXPECT_FALSE(p.managed_snapshots[1].live_bytes);
    ASSERT_EQ(p.managed_survival.size(), 2u);
    EXPECT_EQ(p.managed_survival[0].address, 5678u);
    EXPECT_TRUE(p.managed_survival[0].survived);
    EXPECT_FALSE(p.managed_survival[1].survived);
    EXPECT_TRUE(p.quality.incomplete_capture);
    EXPECT_TRUE(p.quality.sampled_allocations);
    EXPECT_TRUE(p.quality.unresolved_symbols);
    EXPECT_EQ(p.quality.lost_events, 12u);
    EXPECT_EQ(p.quality.warnings, original.profile().quality.warnings);
    EXPECT_EQ(p.source_name, original.profile().source_name);
    EXPECT_EQ(encode(restored), data);
}

TEST(ProfileIo, OpensSavedProfileThroughDesktopLoaderWithoutOriginalFiles) {
    auto original = representative_profile();
    const std::string path = "test_tmp_saved.trprofile";
    std::string error;
    ASSERT_TRUE(write_profile(path, original, error)) << error;
    FileLoader loader;
    QueryDb db;
    loader.load_file(path, false, &db);
    loader.join();
    ASSERT_TRUE(loader.poll_finished());
    std::filesystem::remove(path);
    ASSERT_TRUE(loader.success()) << loader.error();
    auto restored = loader.take_model();
    std::string file;
    int line = 0;
    ASSERT_TRUE(extract_source_location(restored, restored.events()[1], file, line));
    EXPECT_EQ(file, "C:\\sources\\app.cpp");
    EXPECT_EQ(line, 37);
    auto result = db.execute("SELECT kind, sample_count, dur, estimated_cpu_time FROM events WHERE id = 1");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][1], "1");
    EXPECT_EQ(result.rows[0][2], "NULL");
    FlameGraphPanel panel;
    panel.rebuild(restored, ViewState{});
    uint32_t samples = 0;
    for (const auto& tree : panel.trees()) samples += tree.root_sample_count;
    EXPECT_EQ(samples, 3u);
}

TEST(ProfileIo, RejectsUnsupportedVersionAndLeavesExistingModelIntact) {
    auto model = representative_profile();
    auto document = nlohmann::json::parse(encode(model).substr(PROFILE_MAGIC.size()));
    document["version"] = 2;
    auto data = std::string(PROFILE_MAGIC) + document.dump();
    TraceParser parser;
    EXPECT_FALSE(parser.parse_buffer(data.data(), data.size(), model));
    EXPECT_NE(parser.error_message().find("unsupported version 2"), std::string::npos);
    EXPECT_NE(parser.error_message().find("supports version 1"), std::string::npos);
    EXPECT_EQ(model.events().size(), 5u);
}

TEST(ProfileIo, RejectsMalformedIdentitiesUnitsAndLifetimes) {
    const auto valid = nlohmann::json::parse(encode(representative_profile()).substr(PROFILE_MAGIC.size()));
    struct Case {
        const char* name;
        std::function<void(nlohmann::json&)> change;
        const char* error;
    };
    const Case cases[] = {
        {"unit", [](auto& j) { j["time_unit"] = "ns"; }, "requires time_unit us"},
        {"index", [](auto& j) { j["events"][0]["name"] = 999999; }, "events[0]"},
        {"cycle", [](auto& j) { j["stack_frames"][0]["parent"] = j["stack_frames"][1]["id"]; }, "cyclic parent"},
        {"large identifier", [](auto& j) { j["events"][0]["id"] = "18446744073709551616"; }, "64-bit"},
        {"lossy address", [](auto& j) { j["profile"]["allocations"][0]["address"] = 9007199254740992.0; },
         "decimal string"},
        {"duplicate allocation",
         [](auto& j) { j["profile"]["allocations"][1]["id"] = j["profile"]["allocations"][0]["id"]; },
         "duplicate identity"},
        {"missing process", [](auto& j) { j["profile"]["modules"][0]["process_id"] = "absent"; }, "unknown process"},
        {"wrong heap lifetime", [](auto& j) { j["profile"]["allocations"][0]["heap_id"] = "heap-reused"; },
         "different process lifetimes"},
        {"backwards lifetime", [](auto& j) { j["profile"]["allocations"][0]["freed_ts"] = 0; }, "end time precedes"},
        {"invented free", [](auto& j) { j["profile"]["allocations"][1]["freed_ts"] = 8; }, "requires freed"},
        {"bad survival reference",
         [](auto& j) { j["profile"]["managed_survival"][0]["allocation_id"] = "reuse-same-heap"; },
         "unknown managed allocation"},
        {"missing records", [](auto& j) { j.erase("events"); }, "events"},
        {"live before allocation", [](auto& j) { j["profile"]["allocations"][2]["allocated_ts"] = 101; },
         "end time precedes"},
        {"survival after release", [](auto& j) { j["profile"]["allocations"][3]["freed_ts"] = 7; },
         "survival observation follows"},
        {"death before release", [](auto& j) { j["profile"]["allocations"][3]["freed_ts"] = 17; },
         "death observation precedes"},
        {"observation before allocation", [](auto& j) { j["profile"]["allocations"][3]["allocated_ts"] = 9; },
         "GC observation precedes"},
        {"dead and live at capture end",
         [](auto& j) { j["profile"]["allocations"][3]["end_state"] = "live_at_capture_end"; },
         "contradicts live_at_capture_end"},
        {"sort index overflow", [](auto& j) { j["processes"][0]["sort_index"] = UINT64_MAX; }, "32-bit"},
        {"fractional sort index", [](auto& j) { j["processes"][0]["sort_index"] = 1.5; }, "32-bit"},
        {"duplicate empty counters",
         [](auto& j) {
             j["counters"][0]["points"] = nlohmann::json::array();
             j["counters"].push_back(j["counters"][0]);
         },
         "duplicate counter"},
        {"invalid sampled interval", [](auto& j) { j["events"][4]["phase"] = "i"; }, "sampled_span"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        auto changed = valid;
        test.change(changed);
        auto data = std::string(PROFILE_MAGIC) + changed.dump();
        TraceParser parser;
        TraceModel model;
        EXPECT_FALSE(parser.parse_buffer(data.data(), data.size(), model));
        EXPECT_NE(parser.error_message().find(test.error), std::string::npos) << parser.error_message();
        EXPECT_TRUE(model.events().empty());
    }
}

TEST(ProfileIo, PreservesAbsentAllocationHistoryAndSnapshotOnlyTimeRange) {
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    profile.processes = {{"process", 1, std::nullopt, std::nullopt}};
    profile.capabilities.managed_heap_snapshots = true;
    profile.managed_snapshots = {{"first", "process", 100, 1024, 10, {}}, {"last", "process", 200, 2048, 15, {}}};
    profile.quality.warnings = {"Allocation call stacks were not recorded"};
    model.set_profile(std::move(profile));
    auto data = encode(model);
    TraceParser parser;
    TraceModel restored;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), restored)) << parser.error_message();
    EXPECT_TRUE(restored.events().empty());
    EXPECT_TRUE(restored.profile().allocations.empty());
    EXPECT_FALSE(restored.profile().capabilities.native_allocation_history);
    EXPECT_FALSE(restored.profile().capabilities.managed_allocation_history);
    EXPECT_TRUE(restored.profile().capabilities.managed_heap_snapshots);
    EXPECT_DOUBLE_EQ(restored.min_ts(), 100);
    EXPECT_DOUBLE_EQ(restored.max_ts(), 200);
}

TEST(ProfileIo, ChromeImportCanBeSavedAndReopenedWithTransitionsCountersAndFlows) {
    const std::string chrome = R"({"traceEvents":[
        {"name":"task","ph":"B","ts":0,"pid":1,"tid":1},
        {"ph":"E","ts":10,"pid":1,"tid":1},
        {"name":"counter","ph":"C","ts":5,"args":{"bytes":256},"pid":1},
        {"name":"flow","ph":"s","ts":1,"id":"17"},
        {"name":"flow","ph":"f","ts":2,"id":"17"},
        {"ph":"P","ts":3,"sf":"unknown","weight":2,"weightUnit":"samples"}
    ]})";
    TraceParser parser;
    TraceModel original;
    ASSERT_TRUE(parser.parse_buffer(chrome.data(), chrome.size(), original));
    auto data = encode(original);
    TraceModel restored;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), restored)) << parser.error_message();
    ASSERT_EQ(restored.events().size(), original.events().size());
    EXPECT_DOUBLE_EQ(restored.events()[0].dur, 10);
    EXPECT_TRUE(restored.events()[1].is_end_event);
    EXPECT_EQ(restored.counter_series()[0].points, original.counter_series()[0].points);
    EXPECT_EQ(restored.flow_groups(), original.flow_groups());
    EXPECT_LT(restored.events().back().stack_frame_idx, 0);
    EXPECT_EQ(restored.get_string(restored.events().back().stack_frame_id), "unknown");
}
