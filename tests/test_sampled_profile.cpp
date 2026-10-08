#include <gtest/gtest.h>
#include "parser/trace_parser.h"
#include "ui/flame_graph_panel.h"
#include <filesystem>
#include "model/query_db.h"
#include "ui/range_stats.h"
#include "ui/search_panel.h"
#include "ui/event_metrics.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "ui/detail_panel.h"
#include "ui/instance_panel.h"
#include "imgui.h"
#include "imgui_internal.h"

TEST(SampledProfile, KeepsRecursiveBeginEndFrames) {
    const std::string input = R"([
      {"name":"recurse","ph":"B","ts":0,"pid":1,"tid":1},
      {"name":"recurse","ph":"B","ts":0,"pid":1,"tid":1},
      {"ph":"E","ts":10,"pid":1,"tid":1},
      {"ph":"E","ts":10,"pid":1,"tid":1}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    ASSERT_EQ(model.find_thread(1, 1)->event_indices.size(), 2u);
    EXPECT_EQ(model.events()[1].parent_idx, 0);
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 0);
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 10);
}

TEST(SampledProfile, KeepsEqualCompleteIntervalsInInputOrder) {
    const std::string input = R"([
      {"name":"recurse","ph":"X","ts":0,"dur":10,"pid":1,"tid":1},
      {"name":"recurse","ph":"X","ts":0,"dur":10,"pid":1,"tid":1}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    ASSERT_EQ(model.find_thread(1, 1)->event_indices.size(), 2u);
    EXPECT_EQ(model.events()[1].parent_idx, 0);
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 0);
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 10);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 1u);
    const auto& tree = panel.trees()[0];
    EXPECT_DOUBLE_EQ(tree.root_total_time, 10);
    const auto& root = tree.node(tree.first_root);
    EXPECT_DOUBLE_EQ(root.self_time, 0);
    EXPECT_EQ(root.call_count, 1u);
    ASSERT_NE(root.first_child, UINT32_MAX);
    EXPECT_EQ(tree.node(root.first_child).name_idx, root.name_idx);
    EXPECT_DOUBLE_EQ(tree.node(root.first_child).self_time, 10);
    EXPECT_EQ(tree.node(root.first_child).call_count, 1u);
}

static std::filesystem::path fixture_path(const char* name) {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
}

static const FlameTree* tree_for(const FlameGraphPanel& panel, EventKind kind, uint32_t tid) {
    for (const auto& tree : panel.trees())
        if (tree.kind == kind && tree.tid == tid) return &tree;
    return nullptr;
}

TEST(SampledProfile, FieldOrderPreservesIdentityAncestryAndWeights) {
    std::ifstream file(fixture_path("sampled_stacks.json"));
    auto fixture = nlohmann::ordered_json::parse(file);
    for (bool frames_first : {false, true}) {
        nlohmann::ordered_json input;
        if (frames_first) input["stackFrames"] = fixture["stackFrames"];
        input["otherData"] = {{"nested", {{"traceEvents", nlohmann::json::array({1, 2})}}}};
        input["traceEvents"] = fixture["traceEvents"];
        input["ignoredArray"] = nlohmann::json::array({1, {{"name", "ignore me"}}});
        if (!frames_first) input["stackFrames"] = fixture["stackFrames"];
        const auto text = input.dump();
        TraceParser parser;
        TraceModel model;
        ASSERT_TRUE(parser.parse_buffer(text.data(), text.size(), model));
        ASSERT_EQ(model.events().size(), 3u);
        const auto& sample = model.events()[0];
        EXPECT_EQ(sample.kind, EventKind::Sample);
        EXPECT_EQ(sample.pid, 7u);
        EXPECT_EQ(sample.tid, 11u);
        EXPECT_DOUBLE_EQ(sample.ts, 100);
        EXPECT_DOUBLE_EQ(sample.dur, 0);
        EXPECT_DOUBLE_EQ(sample.sample_weight, 250);
        EXPECT_DOUBLE_EQ(sample.sample_cpu_time, 250);
        EXPECT_DOUBLE_EQ(model.events()[1].sample_cpu_time, 500);
        EXPECT_LT(model.events()[2].sample_cpu_time, 0);
        EXPECT_EQ(model.events()[2].tid, 12u);
        auto stack = model.build_sample_stack(0);
        ASSERT_EQ(stack.size(), 2u);
        EXPECT_EQ(model.get_string(model.stack_frames()[stack[0]].name_idx), "main");
        EXPECT_EQ(model.get_string(model.stack_frames()[stack[1]].name_idx), "work");
        EXPECT_EQ(model.stack_frames()[stack[1]].parent_idx, (int32_t)stack[0]);
        EXPECT_EQ(sample.parent_idx, -1);

        FlameGraphPanel panel;
        panel.rebuild(model, ViewState{});
        const auto* tree = tree_for(panel, EventKind::Sample, 11);
        ASSERT_NE(tree, nullptr);
        EXPECT_DOUBLE_EQ(tree->root_total_time, 750);
        EXPECT_EQ(tree->root_sample_count, 2u);
        const auto& root = tree->node(tree->first_root);
        EXPECT_EQ(root.sample_count, 2u);
        EXPECT_EQ(root.self_samples, 1u);
        EXPECT_EQ(root.weighted_samples, 2u);
        EXPECT_DOUBLE_EQ(root.self_time, 500);
        EXPECT_EQ(root.call_count, 0u);
        ASSERT_NE(root.first_child, UINT32_MAX);
        const auto& leaf = tree->node(root.first_child);
        EXPECT_EQ(leaf.sample_count, 1u);
        EXPECT_EQ(leaf.self_samples, 1u);
        EXPECT_DOUBLE_EQ(leaf.total_time, 250);

        auto indices = model.find_thread(7, 11)->event_indices;
        model.build_index();
        EXPECT_EQ(model.find_thread(7, 11)->event_indices, indices);
        EXPECT_EQ(model.build_sample_stack(0), stack);
    }
}

TEST(SampledProfile, InvalidReferencesNeverInventAncestry) {
    const std::string input = R"({
      "stackFrames": {
        "broken":{"name":"broken","parent":"missing"},
        "a":{"name":"a","parent":"b"}, "b":{"name":"b","parent":"a"},
        "descendant":{"name":"descendant","parent":"a"},
        "self":{"name":"self","parent":"self"},
        "badParent":{"name":"badParent","parent":{}},
        "badName":{"name":12}, "notFrame":[],
        "root":{"name":"valid"}
      },
      "traceEvents": [
        {"name":"missing","ph":"P","sf":"missing","ts":1},
        {"name":"broken","ph":"P","sf":"broken","ts":2},
        {"name":"cycle","ph":"P","sf":"a","ts":3},
        {"name":"self","ph":"P","sf":"self","ts":4},
        {"name":"descendant","ph":"P","sf":"descendant","ts":5},
        {"name":"badParent","ph":"P","sf":"badParent","ts":6},
        {"name":"badName","ph":"P","sf":"badName","ts":7},
        {"name":"notFrame","ph":"P","sf":"notFrame","ts":8},
        {"name":"none","ph":"P","ts":9},
        {"name":"invalidType","ph":"P","sf":true,"ts":10},
        {"name":"cpu","ph":"P","sf":"root","ts":11}
      ]})";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    ASSERT_EQ(model.events().size(), 11u);
    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_TRUE(model.build_sample_stack(i).empty()) << i;
        EXPECT_EQ(model.events()[i].stack_frame_idx, -1) << i;
        EXPECT_EQ(model.events()[i].parent_idx, -1) << i;
    }
    EXPECT_EQ(model.build_sample_stack(10).size(), 1u);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 1u);
    EXPECT_EQ(panel.trees()[0].root_sample_count, 11u);
    EXPECT_DOUBLE_EQ(panel.trees()[0].root_total_time, 0);
}

TEST(SampledProfile, NumericIdsAndRecursiveSampleFramesRemainDistinct) {
    const std::string input = R"({"traceEvents":[
      {"ph":"P","ts":1000,"pid":4,"tid":5,"sf":9007199254740993,"weight":2000,"weightUnit":"ns"}],
      "stackFrames":{
        "1":{"name":"recurse"},
        "9007199254740993":{"name":"recurse","parent":1}
      }})";
    TraceParser parser;
    parser.set_time_unit_ns(true);
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_DOUBLE_EQ(model.events()[0].ts, 1);
    EXPECT_DOUBLE_EQ(model.events()[0].sample_cpu_time, 2);
    EXPECT_EQ(model.build_sample_stack(0).size(), 2u);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 1u);
    const auto& tree = panel.trees()[0];
    ASSERT_EQ(tree.nodes.size(), 2u);
    const auto& root = tree.node(tree.first_root);
    ASSERT_NE(root.first_child, UINT32_MAX);
    EXPECT_EQ(root.name_idx, tree.node(root.first_child).name_idx);
    EXPECT_EQ(root.self_samples, 0u);
    EXPECT_EQ(tree.node(root.first_child).self_samples, 1u);
    auto stats = compute_range_stats(model, 0, 10);
    ASSERT_EQ(stats.sample_summaries.size(), 1u);
    EXPECT_EQ(stats.sample_summaries[0].inclusive_samples, 1u);
    EXPECT_EQ(stats.sample_summaries[0].exclusive_samples, 1u);
    EXPECT_DOUBLE_EQ(stats.sample_summaries[0].estimated_cpu_time, 2);
}

TEST(SampledProfile, PerfViewTransitionsKeepRecursionAndEstimatedSemantics) {
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse(fixture_path("perfview_spans.json").string(), model));
    for (int pass = 0; pass < 2; ++pass) {
        const auto* thread = model.find_thread(2, 3);
        ASSERT_NE(thread, nullptr);
        EXPECT_EQ(thread->event_indices, (std::vector<uint32_t>{0, 1, 3}));
        EXPECT_EQ(model.events()[1].parent_idx, 0);
        EXPECT_EQ(model.events()[3].parent_idx, 0);
        EXPECT_DOUBLE_EQ(model.events()[0].self_time, 0);
        EXPECT_DOUBLE_EQ(model.events()[1].dur, 5);
        EXPECT_DOUBLE_EQ(model.events()[3].dur, 5);
        EXPECT_EQ(model.events()[0].kind, EventKind::SampledSpan);
        EXPECT_EQ(model.build_sample_stack(1).size(), 2u);
        FlameGraphPanel panel;
        panel.rebuild(model, ViewState{});
        ASSERT_EQ(panel.trees().size(), 1u);
        const auto& tree = panel.trees()[0];
        EXPECT_EQ(tree.kind, EventKind::SampledSpan);
        EXPECT_DOUBLE_EQ(tree.root_total_time, 10);
        const auto& root = tree.node(tree.first_root);
        EXPECT_EQ(root.call_count, 0u);
        EXPECT_EQ(root.sample_count, 0u);
        EXPECT_EQ(root.span_count, 1u);
        EXPECT_DOUBLE_EQ(root.self_time, 0);
        ASSERT_NE(root.first_child, UINT32_MAX);
        EXPECT_EQ(tree.node(root.first_child).span_count, 2u);
        EXPECT_DOUBLE_EQ(tree.node(root.first_child).total_time, 10);
        model.build_index();
    }
}

TEST(SampledProfile, ZeroLengthTransitionsPreserveExplicitParents) {
    const std::string input = R"([
      {"name":"root","ph":"B","ts":0},
      {"name":"first","ph":"B","ts":10}, {"ph":"E","ts":10},
      {"name":"second","ph":"B","ts":10}, {"ph":"E","ts":10},
      {"ph":"E","ts":10},
      {"name":"nextRoot","ph":"B","ts":10}, {"ph":"E","ts":10}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_EQ(model.events()[1].parent_idx, 0);
    EXPECT_EQ(model.events()[3].parent_idx, 0);
    EXPECT_EQ(model.events()[6].parent_idx, -1);
    EXPECT_EQ(model.build_call_stack(3), (std::vector<uint32_t>{0, 3}));
    for (const auto& event : model.events()) EXPECT_GE(event.self_time, 0);
}

TEST(SampledProfile, SampleRangeUsesObservationTimesAndNeverFillsGaps) {
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse(fixture_path("sampled_stacks.json").string(), model));
    auto empty = compute_range_stats(model, 101, 9999);
    EXPECT_EQ(empty.total_samples, 0u);
    EXPECT_TRUE(empty.sample_summaries.empty());
    auto stats = compute_range_stats(model, 100, 10000);
    EXPECT_EQ(stats.total_samples, 1u);
    EXPECT_EQ(stats.total_events, 0u);
    ASSERT_EQ(stats.sample_summaries.size(), 2u);
    for (const auto& summary : stats.sample_summaries) {
        EXPECT_EQ(summary.inclusive_samples, 1u);
        EXPECT_DOUBLE_EQ(summary.estimated_cpu_time, 250);
        EXPECT_EQ(summary.weighted_samples, 1u);
        EXPECT_EQ(summary.exclusive_samples, model.get_string(summary.name_idx) == "work" ? 1u : 0u);
    }
    ViewState view;
    view.set_range_selection(101, 9999);
    FlameGraphPanel panel;
    panel.rebuild(model, view);
    EXPECT_TRUE(panel.trees().empty());
    view.set_range_selection(100, 10000);
    panel.rebuild(model, view);
    ASSERT_EQ(panel.trees().size(), 1u);
    EXPECT_EQ(panel.trees()[0].root_sample_count, 1u);
    EXPECT_DOUBLE_EQ(panel.trees()[0].root_total_time, 250);
}

TEST(SampledProfile, MixedTraceSeparatesMeasuredSampleAndSpanMetrics) {
    const std::string input = R"([
      {"name":"work","ph":"X","ts":0,"dur":100,"pid":1,"tid":1},
      {"name":"work","cat":"other,sampleEvent","ph":"X","ts":0,"dur":20,"pid":1,"tid":1},
      {"name":"work","ph":"P","ts":1,"pid":1,"tid":1,"weight":5,"weightUnit":"us"},
      {"name":"work","ph":"P","ts":2,"pid":1,"tid":1,"weight":7},
      {"name":"work","ph":"P","ts":3,"pid":1,"tid":1,"dur":1000}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 100);
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 20);
    EXPECT_DOUBLE_EQ(model.events()[4].dur, 0);
    for (const auto& event : model.events()) EXPECT_EQ(event.parent_idx, -1);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 3u);
    const auto* measured = tree_for(panel, EventKind::Measured, 1);
    const auto* sampled = tree_for(panel, EventKind::Sample, 1);
    const auto* span = tree_for(panel, EventKind::SampledSpan, 1);
    ASSERT_NE(measured, nullptr);
    ASSERT_NE(sampled, nullptr);
    ASSERT_NE(span, nullptr);
    EXPECT_DOUBLE_EQ(measured->root_total_time, 100);
    EXPECT_EQ(measured->node(measured->first_root).call_count, 1u);
    EXPECT_DOUBLE_EQ(span->root_total_time, 20);
    EXPECT_EQ(span->node(span->first_root).sample_count, 0u);
    EXPECT_DOUBLE_EQ(sampled->root_total_time, 5);
    EXPECT_EQ(sampled->root_sample_count, 3u);
    EXPECT_EQ(sampled->node(sampled->first_root).weighted_samples, 1u);

    auto stats = compute_range_stats(model, 0, 100);
    EXPECT_EQ(stats.total_events, 1u);
    EXPECT_EQ(stats.total_sampled_spans, 1u);
    EXPECT_EQ(stats.total_samples, 3u);
    ASSERT_EQ(stats.summaries.size(), 2u);
    EXPECT_EQ(stats.summaries[0].kind, EventKind::Measured);
    EXPECT_DOUBLE_EQ(stats.summaries[0].total_dur, 100);
    EXPECT_EQ(stats.summaries[1].kind, EventKind::SampledSpan);
    EXPECT_DOUBLE_EQ(stats.summaries[1].total_dur, 20);
    ASSERT_EQ(stats.sample_summaries.size(), 1u);
    EXPECT_EQ(stats.sample_summaries[0].inclusive_samples, 3u);
    EXPECT_EQ(stats.sample_summaries[0].weighted_samples, 1u);
    EXPECT_DOUBLE_EQ(stats.sample_summaries[0].estimated_cpu_time, 5);

    SearchPanel search;
    search.build_name_stats(model, {0, 1, 2, 3, 4});
    const auto& name = search.name_stats().at(model.events()[0].name_idx);
    EXPECT_EQ(name.count, 1u);
    EXPECT_EQ(name.sample_count, 3u);
    EXPECT_EQ(name.sampled_span_count, 1u);
    EXPECT_DOUBLE_EQ(name.avg_dur, 100);

    QueryDb db;
    db.load(model);
    auto result =
        db.execute("SELECT dur, self_time, sample_count, sample_weight, estimated_cpu_time FROM events ORDER BY id");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.rows.size(), 5u);
    EXPECT_EQ(result.rows[0], (std::vector<std::string>{"100.0", "100.0", "NULL", "NULL", "NULL"}));
    EXPECT_EQ(result.rows[1], (std::vector<std::string>{"NULL", "NULL", "NULL", "NULL", "20.0"}));
    EXPECT_EQ(result.rows[2], (std::vector<std::string>{"NULL", "NULL", "1", "5.0", "5.0"}));
    EXPECT_EQ(result.rows[3], (std::vector<std::string>{"NULL", "NULL", "1", "7.0", "NULL"}));
    EXPECT_EQ(result.rows[4], (std::vector<std::string>{"NULL", "NULL", "1", "NULL", "NULL"}));

    char buf[128];
    format_event_metric(model.events()[0], buf, sizeof(buf));
    EXPECT_EQ(std::string(buf).find("Est."), std::string::npos);
    format_event_metric(model.events()[1], buf, sizeof(buf));
    EXPECT_EQ(std::string(buf).find("Est. CPU"), 0u);
    format_event_metric(model.events()[4], buf, sizeof(buf));
    EXPECT_EQ(std::string(buf), "CPU time unavailable");
}

TEST(SampledProfile, UnknownInvalidAndZeroWeightsStayExplicit) {
    const std::string input = R"([
      {"ph":"P","ts":0,"weight":0,"weightUnit":"us"},
      {"ph":"P","ts":10,"weight":-1,"weightUnit":"us"},
      {"ph":"P","ts":20,"weight":5,"weightUnit":"cycles"},
      {"ph":"P","ts":30,"weight":0.001,"weightUnit":"s"}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_DOUBLE_EQ(model.events()[0].sample_cpu_time, 0);
    EXPECT_LT(model.events()[1].sample_cpu_time, 0);
    EXPECT_LT(model.events()[2].sample_cpu_time, 0);
    EXPECT_DOUBLE_EQ(model.events()[2].sample_weight, 5);
    EXPECT_DOUBLE_EQ(model.events()[3].sample_cpu_time, 1000);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 1u);
    const auto& tree = panel.trees()[0];
    EXPECT_EQ(tree.root_sample_count, 4u);
    EXPECT_DOUBLE_EQ(tree.root_total_time, 1000);
    EXPECT_EQ(tree.node(tree.first_root).weighted_samples, 2u);
}

TEST(SampledProfile, InterleavedMeasuredAndSampledBeginEndPairsStayIndependent) {
    const std::string input = R"([
      {"name":"measured","ph":"B","ts":0},
      {"name":"sampled","ph":"B","cat":"sampleEvent","ts":1},
      {"ph":"E","ts":5},
      {"ph":"E","cat":"sampleEvent","ts":10}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_DOUBLE_EQ(model.events()[0].dur, 5);
    EXPECT_DOUBLE_EQ(model.events()[1].dur, 9);
    EXPECT_EQ(model.events()[0].parent_idx, -1);
    EXPECT_EQ(model.events()[1].parent_idx, -1);
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 5);
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 9);
}

TEST(SampledProfile, CompleteSpansCanNestBetweenBeginEndFrames) {
    const std::string input = R"([
      {"name":"outer","ph":"B","ts":0},
      {"name":"middle","ph":"X","ts":1,"dur":8},
      {"name":"inner","ph":"B","ts":2},
      {"ph":"E","ts":5}, {"ph":"E","ts":10}
    ])";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    EXPECT_EQ(model.build_call_stack(2), (std::vector<uint32_t>{0, 1, 2}));
    EXPECT_DOUBLE_EQ(model.events()[0].self_time, 2);
    EXPECT_DOUBLE_EQ(model.events()[1].self_time, 5);
    EXPECT_DOUBLE_EQ(model.events()[2].self_time, 3);
    FlameGraphPanel panel;
    panel.rebuild(model, ViewState{});
    ASSERT_EQ(panel.trees().size(), 1u);
    const auto& tree = panel.trees()[0];
    EXPECT_DOUBLE_EQ(tree.root_total_time, 10);
    const auto& root = tree.node(tree.first_root);
    EXPECT_DOUBLE_EQ(root.self_time, 2);
    ASSERT_NE(root.first_child, UINT32_MAX);
    const auto& middle = tree.node(root.first_child);
    EXPECT_DOUBLE_EQ(middle.self_time, 5);
}

TEST(SampledProfile, RenderedPanelsIdentifySamplesEstimatesAndMeasuredDurations) {
    const std::string input = R"({"stackFrames":{"root":{"name":"main"},"leaf":{"name":"work","parent":"root"}},
      "traceEvents":[
        {"ph":"P","ts":1,"sf":"leaf","weight":5,"weightUnit":"us"},
        {"ph":"P","ts":2,"sf":"leaf"},
        {"ph":"X","ts":0,"dur":10,"name":"work","cat":"sampleEvent"},
        {"ph":"X","ts":0,"dur":20,"name":"work"}
      ]})";
    TraceParser parser;
    TraceModel model;
    ASSERT_TRUE(parser.parse_buffer(input.data(), input.size(), model));
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ViewState view;
    DetailPanel details;
    FlameGraphPanel flame;
    InstancePanel instances;
    auto render_text = [&](auto& panel) {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        ImGui::SetNextWindowSize(ImVec2(1000, 850));
        panel.render(model, view);
        std::string text = context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::EndFrame();
        return text;
    };
    view.set_selected_event_idx(0);
    auto text = render_text(details);
    EXPECT_NE(text.find("Samples: 1"), std::string::npos) << text;
    EXPECT_NE(text.find("Estimated CPU:"), std::string::npos) << text;
    EXPECT_NE(text.find("main"), std::string::npos) << text;
    EXPECT_EQ(text.find("Wall Time:"), std::string::npos) << text;
    view.set_selected_event_idx(1);
    text = render_text(details);
    EXPECT_NE(text.find("Estimated CPU time unavailable"), std::string::npos) << text;
    view.set_selected_event_idx(2);
    text = render_text(details);
    EXPECT_NE(text.find("Estimated sampled CPU:"), std::string::npos) << text;
    EXPECT_NE(text.find("original sample count is unavailable"), std::string::npos) << text;
    EXPECT_EQ(text.find("Wall Time:"), std::string::npos) << text;
    view.set_selected_event_idx(3);
    text = render_text(details);
    EXPECT_NE(text.find("Wall Time:"), std::string::npos) << text;
    text = render_text(instances);
    EXPECT_NE(text.find("CPU sample"), std::string::npos) << text;
    EXPECT_NE(text.find("Sampled span"), std::string::npos) << text;
    EXPECT_NE(text.find("Measured"), std::string::npos) << text;
    view.set_range_selection(0, 30);
    text = render_text(details);
    EXPECT_NE(text.find("Measured events: 1 | Sampled spans: 1 | Samples: 2"), std::string::npos) << text;
    EXPECT_NE(text.find("Incl. samples"), std::string::npos) << text;
    EXPECT_NE(text.find("Time weights"), std::string::npos) << text;
    text = render_text(flame);
    EXPECT_NE(text.find("CPU sample"), std::string::npos) << text;
    EXPECT_NE(text.find("Sampled span"), std::string::npos) << text;
    EXPECT_NE(text.find("Measured"), std::string::npos) << text;
    ImGui::DestroyContext(context);
}
