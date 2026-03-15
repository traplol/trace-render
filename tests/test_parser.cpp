#include <gtest/gtest.h>
#include "parser/trace_parser.h"
#include <fstream>
#include <filesystem>

class ParserTest : public ::testing::Test {
protected:
    TraceParser parser;
    TraceModel model;

    // Write JSON to a temp file and parse it
    bool parse_json(const std::string& json) {
        std::string path = "test_tmp_trace.json";
        {
            std::ofstream f(path);
            f << json;
        }
        bool result = parser.parse(path, model);
        std::filesystem::remove(path);
        return result;
    }
};

TEST_F(ParserTest, ParsesObjectFormat) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "foo", "ph": "X", "ts": 100, "dur": 50, "pid": 1, "tid": 1, "cat": "test"}
    ]})"));

    // Should have at least one non-metadata event
    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::Complete) {
            found = true;
            EXPECT_DOUBLE_EQ(ev.ts, 100.0);
            EXPECT_DOUBLE_EQ(ev.dur, 50.0);
            EXPECT_EQ(ev.pid, 1u);
            EXPECT_EQ(ev.tid, 1u);
            EXPECT_EQ(model.get_string(ev.name_idx), "foo");
            EXPECT_EQ(model.get_string(ev.cat_idx), "test");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ParsesArrayFormat) {
    ASSERT_TRUE(parse_json(R"([
        {"name": "bar", "ph": "X", "ts": 200, "dur": 30, "pid": 2, "tid": 3, "cat": "c"}
    ])"));

    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::Complete) {
            found = true;
            EXPECT_EQ(model.get_string(ev.name_idx), "bar");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ParsesMetadataProcessName) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "process_name", "ph": "M", "pid": 42, "tid": 0, "args": {"name": "Browser"}},
        {"name": "task", "ph": "X", "ts": 100, "dur": 10, "pid": 42, "tid": 1, "cat": "c"}
    ]})"));

    auto* proc = model.find_process(42);
    ASSERT_NE(proc, nullptr);
    EXPECT_EQ(proc->name, "Browser");
}

TEST_F(ParserTest, ParsesMetadataThreadName) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "thread_name", "ph": "M", "pid": 1, "tid": 5, "args": {"name": "IOThread"}},
        {"name": "task", "ph": "X", "ts": 100, "dur": 10, "pid": 1, "tid": 5, "cat": "c"}
    ]})"));

    auto* proc = model.find_process(1);
    ASSERT_NE(proc, nullptr);
    auto* thread = proc->find_thread(5);
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->name, "IOThread");
}

TEST_F(ParserTest, ParsesCounterEvents) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "Memory", "ph": "C", "ts": 100, "pid": 1, "tid": 0, "cat": "perf", "args": {"value": 42.5}},
        {"name": "Memory", "ph": "C", "ts": 200, "pid": 1, "tid": 0, "cat": "perf", "args": {"value": 55.0}}
    ]})"));

    ASSERT_FALSE(model.counter_series().empty());
    EXPECT_EQ(model.counter_series()[0].name, "Memory");
    EXPECT_EQ(model.counter_series()[0].points.size(), 2u);
}

TEST_F(ParserTest, ParsesFlowEvents) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "flow_start", "ph": "s", "ts": 100, "pid": 1, "tid": 1, "id": 99, "cat": "c"},
        {"name": "flow_end", "ph": "f", "ts": 200, "pid": 2, "tid": 1, "id": 99, "cat": "c"}
    ]})"));

    EXPECT_FALSE(model.flow_groups().empty());
    auto it = model.flow_groups().find(99);
    ASSERT_NE(it, model.flow_groups().end());
    EXPECT_EQ(it->second.size(), 2u);
}

TEST_F(ParserTest, ParsesBEPairs) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "task", "ph": "B", "ts": 100, "pid": 1, "tid": 1, "cat": "c"},
        {"name": "task", "ph": "E", "ts": 250, "pid": 1, "tid": 1, "cat": "c"}
    ]})"));

    // After build_index, B event should have dur=150
    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::DurationBegin) {
            EXPECT_DOUBLE_EQ(ev.dur, 150.0);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ParsesArgsAsJson) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "ev", "ph": "X", "ts": 100, "dur": 10, "pid": 1, "tid": 1, "cat": "c",
         "args": {"key1": "value1", "key2": 123}}
    ]})"));

    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::Complete && ev.args_idx != UINT32_MAX) {
            found = true;
            EXPECT_FALSE(model.args()[ev.args_idx].empty());
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ParsesTimestampsAsNanoseconds) {
    parser.set_time_unit_ns(true);
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "ev", "ph": "X", "ts": 5000, "dur": 2000, "pid": 1, "tid": 1, "cat": "c"}
    ]})"));

    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::Complete) {
            found = true;
            // 5000 ns = 5.0 us, 2000 ns = 2.0 us
            EXPECT_DOUBLE_EQ(ev.ts, 5.0);
            EXPECT_DOUBLE_EQ(ev.dur, 2.0);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ParsesTimestampsAsMicrosecondsByDefault) {
    ASSERT_TRUE(parse_json(R"({"traceEvents": [
        {"name": "ev", "ph": "X", "ts": 5000, "dur": 2000, "pid": 1, "tid": 1, "cat": "c"}
    ]})"));

    bool found = false;
    for (const auto& ev : model.events()) {
        if (ev.ph == Phase::Complete) {
            found = true;
            EXPECT_DOUBLE_EQ(ev.ts, 5000.0);
            EXPECT_DOUBLE_EQ(ev.dur, 2000.0);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, InvalidFileReturnsError) {
    EXPECT_FALSE(parser.parse("/nonexistent/file.json", model));
    EXPECT_FALSE(parser.error_message().empty());
}

TEST_F(ParserTest, ParsesTestTraceFile) {
    ASSERT_TRUE(parser.parse("test_trace.json", model));

    // Verify processes
    auto* browser = model.find_process(1);
    ASSERT_NE(browser, nullptr);
    EXPECT_EQ(browser->name, "Browser");

    auto* renderer = model.find_process(2);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->name, "Renderer");

    // Verify time range is reasonable
    EXPECT_LT(model.min_ts(), model.max_ts());

    // Verify counter series parsed
    EXPECT_FALSE(model.counter_series().empty());

    // Verify flow events parsed
    EXPECT_FALSE(model.flow_groups().empty());

    // Verify events exist
    EXPECT_GT(model.events().size(), 10u);
}
