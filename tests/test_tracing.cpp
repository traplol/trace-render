#include <gtest/gtest.h>
#include "tracing.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <limits>

using json = nlohmann::json;

// --- make_args_json unit tests ---

TEST(MakeArgsJson, Empty) {
    auto result = trace_detail::make_args_json();
    EXPECT_EQ(result, "");
}

TEST(MakeArgsJson, SingleString) {
    auto result = trace_detail::make_args_json("key", "hello");
    EXPECT_EQ(result, R"("key":"hello")");
}

TEST(MakeArgsJson, MultiplePairs) {
    auto result = trace_detail::make_args_json("a", 1, "b", "two");
    EXPECT_EQ(result, R"("a":1,"b":"two")");
}

TEST(MakeArgsJson, BoolValues) {
    auto result = trace_detail::make_args_json("t", true, "f", false);
    EXPECT_EQ(result, R"("t":true,"f":false)");
}

TEST(MakeArgsJson, IntTypes) {
    auto result = trace_detail::make_args_json("i", 42, "l", 100L, "ll", 200LL, "u", 300U, "ul", 400UL, "ull", 500ULL);
    EXPECT_EQ(result, R"("i":42,"l":100,"ll":200,"u":300,"ul":400,"ull":500)");
}

TEST(MakeArgsJson, FloatDouble) {
    auto result = trace_detail::make_args_json("f", 1.5f, "d", 2.5);
    EXPECT_EQ(result, R"("f":1.5,"d":2.5)");
}

TEST(MakeArgsJson, NanInf) {
    float nan_f = std::numeric_limits<float>::quiet_NaN();
    double inf_d = std::numeric_limits<double>::infinity();
    double neg_inf = -std::numeric_limits<double>::infinity();
    auto result = trace_detail::make_args_json("nan", nan_f, "inf", inf_d, "ninf", neg_inf);
    EXPECT_EQ(result, R"("nan":null,"inf":null,"ninf":null)");
}

TEST(MakeArgsJson, NullCString) {
    const char* p = nullptr;
    auto result = trace_detail::make_args_json("key", p);
    EXPECT_EQ(result, R"("key":null)");
}

TEST(MakeArgsJson, StdString) {
    std::string val = "world";
    auto result = trace_detail::make_args_json("hello", val);
    EXPECT_EQ(result, R"("hello":"world")");
}

TEST(MakeArgsJson, StringEscaping) {
    auto result = trace_detail::make_args_json("msg", "he said \"hello\"\nand\\left");
    // Should produce: "msg":"he said \"hello\"\nand\\left"
    EXPECT_EQ(result, R"("msg":"he said \"hello\"\nand\\left")");
}

TEST(MakeArgsJson, KeyEscaping) {
    auto result = trace_detail::make_args_json("key\"with\"quotes", 1);
    EXPECT_EQ(result, R"("key\"with\"quotes":1)");
}

TEST(MakeArgsJson, ControlCharEscaping) {
    std::string val(1, '\x01');
    val += '\x1f';
    auto result = trace_detail::make_args_json("ctrl", val);
    EXPECT_EQ(result, R"("ctrl":"\u0001\u001f")");
}

TEST(MakeArgsJson, ValidJson) {
    // Verify the output can be parsed as a JSON object when wrapped in braces
    auto inner = trace_detail::make_args_json("name", "test\"func", "count", 42, "ratio", 3.14, "ok", true, "msg",
                                              "line1\nline2");
    std::string full = "{" + inner + "}";
    auto j = json::parse(full);
    EXPECT_EQ(j["name"], "test\"func");
    EXPECT_EQ(j["count"], 42);
    EXPECT_NEAR(j["ratio"].get<double>(), 3.14, 0.001);
    EXPECT_EQ(j["ok"], true);
    EXPECT_EQ(j["msg"], "line1\nline2");
}

TEST(MakeArgsJson, NegativeNumbers) {
    auto result = trace_detail::make_args_json("neg", -42, "negf", -1.5);
    EXPECT_EQ(result, R"("neg":-42,"negf":-1.5)");
}

TEST(MakeArgsJson, LargeNumbers) {
    auto result = trace_detail::make_args_json("big", 9223372036854775807LL, "ubig", 18446744073709551615ULL);
    auto full = "{" + result + "}";
    auto j = json::parse(full);
    EXPECT_EQ(j["big"], 9223372036854775807LL);
    EXPECT_EQ(j["ubig"], 18446744073709551615ULL);
}

// --- Integration tests: write trace, parse back ---

class TracingIntegration : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override { tmp_path_ = "/tmp/test_tracing_" + std::to_string(getpid()) + ".json"; }
    void TearDown() override { std::remove(tmp_path_.c_str()); }

    json read_trace() {
        std::ifstream f(tmp_path_);
        return json::parse(f);
    }
};

TEST_F(TracingIntegration, WriteCompleteNoArgs) {
    Tracer::instance().set_output(tmp_path_);
    Tracer::instance().write_complete("test_func", "test", 1000, 500);
    Tracer::instance().close();

    auto j = read_trace();
    ASSERT_EQ(j["traceEvents"].size(), 1);
    auto& ev = j["traceEvents"][0];
    EXPECT_EQ(ev["name"], "test_func");
    EXPECT_EQ(ev["cat"], "test");
    EXPECT_EQ(ev["ts"], 1000);
    EXPECT_EQ(ev["dur"], 500);
    EXPECT_FALSE(ev.contains("args"));
}

TEST_F(TracingIntegration, WriteCompleteWithArgs) {
    Tracer::instance().set_output(tmp_path_);
    auto args = trace_detail::make_args_json("count", 42, "label", "hello \"world\"");
    Tracer::instance().write_complete("func", "cat", 2000, 100, args.c_str());
    Tracer::instance().close();

    auto j = read_trace();
    ASSERT_EQ(j["traceEvents"].size(), 1);
    auto& ev = j["traceEvents"][0];
    EXPECT_EQ(ev["name"], "func");
    EXPECT_EQ(ev["args"]["count"], 42);
    EXPECT_EQ(ev["args"]["label"], "hello \"world\"");
}

TEST_F(TracingIntegration, WriteCompleteEmptyArgs) {
    Tracer::instance().set_output(tmp_path_);
    Tracer::instance().write_complete("func", "cat", 3000, 50, "");
    Tracer::instance().close();

    auto j = read_trace();
    auto& ev = j["traceEvents"][0];
    EXPECT_FALSE(ev.contains("args"));
}

TEST_F(TracingIntegration, TraceScopeArgsDisabled) {
    // Tracing not enabled — should not crash, should not allocate
    { TRACE_SCOPE_ARGS("func", "cat", "key", 123); }
    // Just verify no crash
}

TEST_F(TracingIntegration, TraceScopeArgsEnabled) {
    Tracer::instance().set_output(tmp_path_);
    { TRACE_SCOPE_ARGS("my_func", "my_cat", "iterations", 1000, "name", "test"); }
    Tracer::instance().close();

    auto j = read_trace();
    ASSERT_GE(j["traceEvents"].size(), 1);
    auto& ev = j["traceEvents"][0];
    EXPECT_EQ(ev["name"], "my_func");
    EXPECT_EQ(ev["cat"], "my_cat");
    EXPECT_EQ(ev["args"]["iterations"], 1000);
    EXPECT_EQ(ev["args"]["name"], "test");
    EXPECT_GE(ev["dur"].get<int>(), 0);  // dur can be 0 if scope completes within 1us
}

TEST_F(TracingIntegration, MultipleEvents) {
    Tracer::instance().set_output(tmp_path_);
    Tracer::instance().write_complete("a", "c", 100, 10);
    { TRACE_SCOPE_ARGS("b", "c", "x", 1); }
    Tracer::instance().write_complete("c", "c", 200, 20, trace_detail::make_args_json("flag", true).c_str());
    Tracer::instance().close();

    auto j = read_trace();
    EXPECT_EQ(j["traceEvents"].size(), 3);
    // All should be valid JSON
    for (auto& ev : j["traceEvents"]) {
        EXPECT_TRUE(ev.contains("name"));
        EXPECT_TRUE(ev.contains("ph"));
    }
}
