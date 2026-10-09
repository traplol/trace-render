#include <gtest/gtest.h>
#include "platform/file_loader.h"
#include "parser/profile_io.h"
#include "parser/trace_parser.h"
#include "symbols/native_symbol_resolver.h"
#include <filesystem>
#include <cstdlib>
#include <fstream>

namespace {
std::vector<char> buffer(const std::string& text) {
    return {text.begin(), text.end()};
}
void finish(FileLoader& loader) {
    loader.join();
    ASSERT_TRUE(loader.poll_finished());
    EXPECT_FALSE(loader.is_loading());
    EXPECT_FALSE(loader.poll_finished());
}
}  // namespace

TEST(FileLoader, CancelDiscardsPartialModelAndAllowsNextLoad) {
    std::string trace = "[";
    for (size_t i = 0; i < 100000; ++i) {
        if (i) trace += ',';
        trace += R"({"ph":"X","name":"operation","pid":1,"tid":2,"ts":1,"dur":2})";
    }
    trace += ']';
    FileLoader loader;
    loader.load_buffer(buffer(trace), "long.json", false);
    loader.cancel();
    finish(loader);
    EXPECT_FALSE(loader.success());
    EXPECT_EQ(loader.error(), "Import canceled");
    EXPECT_TRUE(loader.take_model().events().empty());
    loader.load_buffer(buffer(R"([{"ph":"X","name":"next","pid":1,"tid":2,"ts":1,"dur":2}])"), "next.json", false);
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    EXPECT_EQ(loader.take_model().events().size(), 1u);
    loader.load_buffer(buffer("broken"), "broken.json", false);
    finish(loader);
    EXPECT_FALSE(loader.success());
    EXPECT_FALSE(loader.error().empty());
    EXPECT_TRUE(loader.take_model().events().empty());
}

TEST(FileLoader, SymbolsResolveDuringLoadAndSavedNamesSurviveMissingPdbs) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    profile.source_format = "diagsession";
    profile.capture_start_ts = 0;
    profile.capture_end_ts = 100;
    profile.processes = {{"process", 7, 0, 100}};
    ProfileModule module;
    module.id = "module";
    module.process_id = "process";
    module.name = "fixture.exe";
    module.build_id = "5C6542D2-6C0D-3468-4C4C-44205044422E/1";
    module.load_address = 0x140000000;
    module.size_bytes = 0x4000;
    module.pdb_path = "fixture.pdb";
    profile.modules = {module};
    profile.capabilities.native_cpu_samples = true;
    StackFrame frame;
    frame.id_idx = model.intern_string("frame");
    frame.module_id = model.intern_string(module.id);
    frame.address = module.load_address + 0x1005;
    frame.name_idx = model.intern_string("unknown");
    model.add_stack_frame(frame);
    TraceEvent sample;
    sample.ph = Phase::Sample;
    sample.kind = EventKind::Sample;
    sample.ts = 1;
    sample.pid = 7;
    sample.tid = 8;
    sample.process_instance_id = model.intern_string("process");
    sample.stack_frame_id = frame.id_idx;
    model.add_event(sample);
    model.set_profile(profile);
    model.build_index();
    std::string saved, error;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    FileLoader loader;
    loader.set_symbol_paths({(std::filesystem::path(__FILE__).parent_path() / "fixtures/native_symbols").string()});
    loader.load_buffer(buffer(saved), "captured.trprofile", false);
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    model = loader.take_model();
    ASSERT_TRUE(model.stack_frames()[0].symbol_resolved);
    EXPECT_EQ(model.get_string(model.stack_frames()[0].name_idx), "allocate_buffer");
    EXPECT_EQ(model.stack_frames()[0].source_line, 3u);
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    loader.set_symbol_paths({"/unavailable-symbol-directory"});
    loader.load_buffer(buffer(saved), "shared.trprofile", false);
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    model = loader.take_model();
    EXPECT_TRUE(model.stack_frames()[0].symbol_resolved);
    EXPECT_EQ(model.get_string(model.stack_frames()[0].name_idx), "allocate_buffer");
    EXPECT_EQ(model.stack_frames()[0].source_line, 3u);
}

TEST(FileLoader, AuthenticNativeCapturesUseDesktopPathAndReopenWithoutOriginals) {
    const char* cpu = std::getenv("TRACE_NATIVE_CPU_FIXTURE");
    const char* memory = std::getenv("TRACE_NATIVE_MEMORY_FIXTURE");
    if (!cpu || !memory) GTEST_SKIP() << "Set TRACE_NATIVE_CPU_FIXTURE and TRACE_NATIVE_MEMORY_FIXTURE";
    FileLoader loader;
    if (const char* symbols = std::getenv("TRACE_RENDER_OBS_SYMBOLS")) loader.set_symbol_paths({symbols});
    loader.load_file(cpu, false);
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    auto model = loader.take_model();
    EXPECT_EQ(model.events().size(), 25232u);
    if (NativeSymbolResolver::available() && std::getenv("TRACE_RENDER_OBS_SYMBOLS")) {
        size_t resolved = 0;
        for (const auto& frame : model.stack_frames())
            if (frame.symbol_resolved) ++resolved;
        EXPECT_GT(resolved, 0u);
    }
    model.clear();
    loader.set_symbol_paths({});
    loader.load_file(memory, false);
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    model = loader.take_model();
    ASSERT_EQ(model.profile().allocations.size(), 311635u);
    ASSERT_TRUE(model.profile().capture_end_ts);
    const auto end = *model.profile().capture_end_ts;
    auto totals = model.query_outstanding_memory(end);
    EXPECT_EQ(totals.total.known.count, 4502u);
    EXPECT_EQ(totals.total.known.bytes, 1068330u);
    std::string saved, error;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    model.clear();
    loader.load_buffer(buffer(saved), "shared.trprofile", false);
    saved.clear();
    saved.shrink_to_fit();
    finish(loader);
    ASSERT_TRUE(loader.success()) << loader.error();
    model = loader.take_model();
    EXPECT_EQ(model.profile().allocations.size(), 311635u);
    totals = model.query_outstanding_memory(end);
    EXPECT_EQ(totals.total.known.count, 4502u);
    EXPECT_EQ(totals.total.known.bytes, 1068330u);
}
