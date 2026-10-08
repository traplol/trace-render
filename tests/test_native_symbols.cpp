#include <gtest/gtest.h>
#include "symbols/native_symbol_resolver.h"
#include "parser/profile_io.h"
#include "parser/trace_parser.h"
#include "ui/flame_graph_panel.h"
#include "ui/range_stats.h"
#include "ui/source_panel.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path fixture() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures/native_symbols/fixture.pdb";
}

ProfileModule fixture_module() {
    ProfileModule module;
    module.id = "process-1/module-1";
    module.process_id = "process-1";
    module.name = "application.exe";  // The recorded PDB filename deliberately differs.
    module.path = "C:\\captured\\application.exe";
    module.pdb_path = "C:\\build\\fixture.pdb";
    module.build_id = "{5C6542D2-6C0D-3468-4C4C-44205044422E}/1";
    module.load_address = UINT64_C(0x140000000);
    module.size_bytes = 0x4000;
    module.load_ts = 10;
    module.unload_ts = 100;
    return module;
}

TraceModel native_profile() {
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    auto module = fixture_module();
    profile.processes = {{module.process_id, 7, 0, 100}};
    profile.heaps = {{"heap", module.process_id, 0, 100}};
    profile.modules = {module};
    profile.capabilities.native_cpu_samples = true;
    profile.capabilities.native_allocation_history = true;
    profile.allocations = {{"allocation", module.process_id, "heap", 1234, 64, "allocation-frame", "",
                            AllocationKind::Native, 30, std::nullopt, AllocationEnd::Unknown}};
    StackFrame frame;
    frame.id_idx = model.intern_string("cpu-frame");
    frame.module_id = model.intern_string(module.id);
    frame.address = module.load_address + 0x1005;
    model.add_stack_frame(frame);
    frame.id_idx = model.intern_string("allocation-frame");
    frame.address = module.load_address + 0x100f;
    model.add_stack_frame(frame);
    TraceEvent sample;
    sample.ph = Phase::Sample;
    sample.kind = EventKind::Sample;
    sample.pid = 7;
    sample.tid = 8;
    sample.ts = 20;
    sample.process_instance_id = model.intern_string(module.process_id);
    sample.stack_frame_id = model.stack_frames()[0].id_idx;
    model.add_event(sample);
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}
}  // namespace

TEST(NativeSymbols, ResolvesMatchingPdbAndSourceWithoutBinary) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    NativeSymbolResolver resolver({fixture().parent_path().string()});
    auto module = fixture_module();
    auto symbol = resolver.resolve(module, module.load_address + 0x1005, 20);
    ASSERT_TRUE(symbol.resolved) << symbol.diagnostic;
    EXPECT_EQ(symbol.name, "allocate_buffer");
    EXPECT_EQ(symbol.source_file, "/trace-render-fixture/tests/fixtures/native_symbols/fixture.c");
    EXPECT_EQ(symbol.source_line, 3u);
    EXPECT_EQ(symbol.symbol_id, module.id + "/fn/0x1000");
    auto other_line = resolver.resolve(module, module.load_address + 0x100f, 20);
    EXPECT_TRUE(other_line.resolved);
    EXPECT_EQ(other_line.source_line, 4u);
    EXPECT_EQ(other_line.symbol_id, symbol.symbol_id);
}

TEST(NativeSymbols, SuppliedBinaryStillRequiresCapturedPdbIdentity) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    auto binary = fixture();
    binary.replace_extension(".exe");
    NativeSymbolResolver resolver({binary.string()});
    auto module = fixture_module();
    auto symbol = resolver.resolve(module, module.load_address + 0x1005);
    ASSERT_TRUE(symbol.resolved) << symbol.diagnostic;
    EXPECT_EQ(symbol.name, "allocate_buffer");
    module.build_id.clear();
    EXPECT_FALSE(resolver.resolve(module, module.load_address + 0x1005).resolved);
}

TEST(NativeSymbols, CorruptCandidateDoesNotBlockOtherMatchingSymbols) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    auto path = std::filesystem::temp_directory_path() / "trace-render-invalid.pdb";
    {
        std::ofstream file(path);
        file << "not a PDB";
    }
    NativeSymbolResolver resolver({path.string(), fixture().string()});
    auto module = fixture_module();
    auto symbol = resolver.resolve(module, module.load_address + 0x1005);
    std::filesystem::remove(path);
    ASSERT_TRUE(symbol.resolved) << symbol.diagnostic;
    EXPECT_EQ(symbol.name, "allocate_buffer");
}

TEST(NativeSymbols, RejectsWrongGuidAgeAndUnverifiableBuild) {
    NativeSymbolResolver resolver({fixture().string()});
    for (const char* identity :
         {"00000000-6C0D-3468-4C4C-44205044422E/1", "5C6542D2-6C0D-3468-4C4C-44205044422E/2", ""}) {
        auto module = fixture_module();
        module.build_id = identity;
        auto symbol = resolver.resolve(module, module.load_address + 0x1005, 20);
        EXPECT_FALSE(symbol.resolved);
        EXPECT_EQ(symbol.name, "application.exe+0x1005");
        EXPECT_TRUE(symbol.source_file.empty());
        EXPECT_EQ(symbol.source_line, 0u);
        EXPECT_FALSE(symbol.diagnostic.empty());
    }
}

TEST(NativeSymbols, MissingSymbolsAndUnavailableBackendKeepOffsets) {
    NativeSymbolResolver resolver({"/trace-render-nonexistent/symbols.pdb"});
    auto module = fixture_module();
    auto symbol = resolver.resolve(module, module.load_address + 0x1005, 20);
    EXPECT_FALSE(symbol.resolved);
    EXPECT_EQ(symbol.name, "application.exe+0x1005");
    EXPECT_EQ(symbol.symbol_id, module.id + "/ip/0x140001005");
    if (!NativeSymbolResolver::available())
        EXPECT_NE(symbol.diagnostic.find("unavailable in this build"), std::string::npos);
}

TEST(NativeSymbols, RespectsModuleRangeAndLifetimeAtReusedAddresses) {
    NativeSymbolResolver resolver({fixture().string()});
    auto first = fixture_module();
    auto second = first;
    second.id = "process-2/module-1";
    second.process_id = "process-2";
    second.load_ts = 100;
    second.unload_ts = std::nullopt;
    for (double timestamp : {9.0, 100.0}) {
        auto rejected = resolver.resolve(first, first.load_address + 0x1005, timestamp);
        EXPECT_FALSE(rejected.resolved);
        EXPECT_NE(rejected.diagnostic.find("lifetime"), std::string::npos);
    }
    auto outside = resolver.resolve(first, first.load_address + first.size_bytes, 20);
    EXPECT_FALSE(outside.resolved);
    EXPECT_NE(outside.diagnostic.find("range"), std::string::npos);
    auto before = resolver.resolve(first, first.load_address - 1, 20);
    EXPECT_FALSE(before.resolved);
    EXPECT_EQ(before.name, "0x13fffffff");
    auto a = resolver.resolve(first, first.load_address + 0x1005, 20);
    auto b = resolver.resolve(second, second.load_address + 0x1005, 100);
    EXPECT_NE(a.symbol_id, b.symbol_id);
    EXPECT_EQ(a.resolved, NativeSymbolResolver::available());
    EXPECT_EQ(b.resolved, NativeSymbolResolver::available());
}

TEST(NativeSymbols, ReusesOpenedPdbAfterItsPathIsRemoved) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    auto path = std::filesystem::temp_directory_path() / "trace-render-symbol-cache.embedded-resource";
    std::filesystem::copy_file(fixture(), path, std::filesystem::copy_options::overwrite_existing);
    NativeSymbolResolver resolver({path.string()});
    auto module = fixture_module();
    auto first = resolver.resolve(module, module.load_address + 0x1005);
    std::filesystem::remove(path);
    ASSERT_TRUE(first.resolved) << first.diagnostic;
    EXPECT_TRUE(resolver.resolve(module, module.load_address + 0x1005).resolved);
    auto second = resolver.resolve(module, module.load_address + 0x1025);
    ASSERT_TRUE(second.resolved) << second.diagnostic;
    EXPECT_EQ(second.name, "release_buffer");
    EXPECT_EQ(second.source_line, 8u);
}

TEST(NativeSymbols, SharedCpuAndAllocationFramesSaveResolvedWithoutOriginalPdb) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    auto path = std::filesystem::temp_directory_path() / "trace-render-shared-resolver.pdb";
    std::filesystem::copy_file(fixture(), path, std::filesystem::copy_options::overwrite_existing);
    auto model = native_profile();
    {
        NativeSymbolResolver resolver({path.string()});
        resolver.resolve_profile(model);
    }
    std::filesystem::remove(path);
    ASSERT_TRUE(model.stack_frames()[0].symbol_resolved);
    ASSERT_TRUE(model.stack_frames()[1].symbol_resolved);
    EXPECT_EQ(model.stack_frames()[0].symbol_id, model.stack_frames()[1].symbol_id);
    EXPECT_EQ(model.get_string(model.events()[0].name_idx), "allocate_buffer");
    std::string data, error;
    ASSERT_TRUE(serialize_profile(model, data, error)) << error;
    TraceParser parser;
    TraceModel restored;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), restored)) << parser.error_message();
    NativeSymbolResolver empty_resolver;
    empty_resolver.resolve_profile(restored);  // Reopening does not erase saved successful resolutions.
    EXPECT_TRUE(restored.stack_frames()[0].symbol_resolved);
    EXPECT_TRUE(restored.stack_frames()[1].symbol_resolved);
    EXPECT_EQ(restored.profile().allocations[0].stack_frame_id, "allocation-frame");
    std::string file;
    int line = 0;
    ASSERT_TRUE(extract_source_location(restored, restored.events()[0], file, line));
    EXPECT_EQ(line, 3);
    EXPECT_EQ(restored.profile().modules[0].pdb_path, "C:\\build\\fixture.pdb");
}

TEST(NativeSymbols, UnresolvedSharedFramesPreserveIdentityAndReportQualityOnce) {
    auto model = native_profile();
    NativeSymbolResolver resolver;
    resolver.resolve_profile(model);
    resolver.resolve_profile(model);
    EXPECT_FALSE(model.stack_frames()[0].symbol_resolved);
    EXPECT_EQ(model.get_string(model.events()[0].name_idx), "application.exe+0x1005");
    EXPECT_TRUE(model.stack_frames()[0].address);
    EXPECT_NE(model.stack_frames()[0].symbol_id, UINT32_MAX);
    EXPECT_TRUE(model.profile().quality.unresolved_symbols);
    EXPECT_EQ(model.profile().quality.warnings.size(), 1u);
}

TEST(NativeSymbols, DoesNotInventNamesInPdbGaps) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    NativeSymbolResolver resolver({fixture().string()});
    auto module = fixture_module();
    auto symbol = resolver.resolve(module, module.load_address + 0x1019);
    EXPECT_FALSE(symbol.resolved);
    EXPECT_EQ(symbol.name, "application.exe+0x1019");
    EXPECT_NE(symbol.diagnostic.find("No function record"), std::string::npos);
}

TEST(NativeSymbols, KeepsSameNameFunctionsSeparateInFlameAndRangeViews) {
    auto model = native_profile();
    auto first = model.stack_frames()[0];
    auto second = model.stack_frames()[1];
    model.set_stack_frame_symbol(0, "same_name", "module-first/function", "", 0, true);
    model.set_stack_frame_symbol(1, "same_name", "module-second/function", "", 0, true);
    TraceEvent sample = model.events()[0];
    sample.ts = 25;
    sample.stack_frame_id = second.id_idx;
    model.add_event(sample);
    // Another observed address in the first function merges by symbol ID, not stack-frame ID.
    auto third = first;
    third.id_idx = model.intern_string("another-call-site");
    model.add_stack_frame(third);
    model.set_stack_frame_symbol(2, "same_name", "module-first/function", "", 0, true);
    sample.ts = 30;
    sample.stack_frame_id = third.id_idx;
    model.add_event(sample);
    model.build_index();
    FlameGraphPanel flame;
    flame.rebuild(model, ViewState{});
    ASSERT_EQ(flame.trees().size(), 1u);
    const auto& tree = flame.trees()[0];
    ASSERT_EQ(tree.nodes.size(), 2u);
    EXPECT_EQ(tree.root_sample_count, 3u);
    EXPECT_EQ(tree.nodes[tree.first_root].sample_count, 2u);
    EXPECT_EQ(tree.nodes[tree.nodes[tree.first_root].next_sibling].sample_count, 1u);
    auto range = compute_range_stats(model, 0, 100);
    ASSERT_EQ(range.sample_summaries.size(), 2u);
    EXPECT_EQ(range.sample_summaries[0].inclusive_samples, 2u);
    EXPECT_EQ(range.sample_summaries[1].inclusive_samples, 1u);
}

TEST(NativeSymbols, RecursiveNativeFunctionCountsOnceWithoutMergingItsSameNameCallee) {
    TraceModel model;
    model.intern_string("");
    StackFrame frame;
    frame.name_idx = model.intern_string("same_name");
    const uint32_t function_a = model.intern_string("module/function-a");
    const uint32_t function_b = model.intern_string("module/function-b");
    for (int i = 0; i < 3; ++i) {
        frame.id_idx = model.intern_string("frame-" + std::to_string(i));
        frame.symbol_id = i == 1 ? function_b : function_a;
        model.add_stack_frame(frame);
        frame.parent_id = frame.id_idx;
    }
    TraceEvent sample;
    sample.ph = Phase::Sample;
    sample.kind = EventKind::Sample;
    sample.ts = 20;
    sample.stack_frame_id = frame.id_idx;
    model.add_event(sample);
    model.build_index();
    FlameGraphPanel flame;
    flame.rebuild(model, ViewState{});
    ASSERT_EQ(flame.trees().size(), 1u);
    EXPECT_EQ(flame.trees()[0].nodes.size(), 3u);
    auto range = compute_range_stats(model, 0, 100);
    ASSERT_EQ(range.sample_summaries.size(), 2u);
    uint32_t inclusive = 0, exclusive = 0;
    for (const auto& summary : range.sample_summaries) {
        inclusive += summary.inclusive_samples;
        exclusive += summary.exclusive_samples;
    }
    EXPECT_EQ(inclusive, 2u);
    EXPECT_EQ(exclusive, 1u);
}

TEST(NativeSymbols, AuthenticObsQtAddressesResolveWithCapturedIdentities) {
    const char* directory = std::getenv("TRACE_RENDER_OBS_SYMBOLS");
    if (!directory || !NativeSymbolResolver::available())
        GTEST_SKIP() << "Set TRACE_RENDER_OBS_SYMBOLS for public OBS integration verification";
    NativeSymbolResolver resolver({directory});
    ProfileModule widgets;
    widgets.id = "obs-1712/qt-widgets";
    widgets.process_id = "obs-1712";
    widgets.name = "Qt6Widgets.dll";
    widgets.build_id = "1A6ED01C-6CE2-40AE-8959-60A52DAEBE62/1";
    widgets.load_address = UINT64_C(0x7ffbd1560000);
    widgets.size_bytes = 0x62c000;  // Captured Image-v3 SizeOfImage, not an inferred bound.
    const std::vector<std::tuple<uint32_t, std::string, uint32_t>> cases = {
        {0x5495c, "QWidgetPrivate::drawWidget", 5658},
        {0x5e3f6, "QWidgetPrivate::paintSiblingsRecursive", 5794},
        {0x12056, "QApplication::notify", 3250}};
    for (const auto& [rva, name, line] : cases) {
        auto symbol = resolver.resolve(widgets, widgets.load_address + rva);
        ASSERT_TRUE(symbol.resolved) << symbol.diagnostic;
        EXPECT_EQ(symbol.name, name);
        EXPECT_EQ(symbol.source_line, line);
        EXPECT_FALSE(symbol.source_file.empty());
    }
}
