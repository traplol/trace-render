#include <gtest/gtest.h>
#include "ui/source_panel.h"

static TraceModel make_model_with_args(const std::string& args_json) {
    TraceModel model;
    model.intern_string("");  // idx 0
    model.args_.push_back(args_json);
    TraceEvent ev{};
    ev.args_idx = 0;
    model.events_.push_back(ev);
    return model;
}

TEST(ExtractSourceLocation, FileAndLine) {
    auto model = make_model_with_args(R"({"file":"src/main.cpp","line":42})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events_[0], file, line));
    EXPECT_EQ(file, "src/main.cpp");
    EXPECT_EQ(line, 42);
}

TEST(ExtractSourceLocation, SrcFileSrcLine) {
    auto model = make_model_with_args(R"({"src_file":"foo.h","src_line":10})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events_[0], file, line));
    EXPECT_EQ(file, "foo.h");
    EXPECT_EQ(line, 10);
}

TEST(ExtractSourceLocation, FileNameLineNumber) {
    auto model = make_model_with_args(R"({"fileName":"bar.cc","lineNumber":99})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events_[0], file, line));
    EXPECT_EQ(file, "bar.cc");
    EXPECT_EQ(line, 99);
}

TEST(ExtractSourceLocation, FileOnly) {
    auto model = make_model_with_args(R"({"file":"only_file.cpp"})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events_[0], file, line));
    EXPECT_EQ(file, "only_file.cpp");
    EXPECT_EQ(line, -1);
}

TEST(ExtractSourceLocation, NoSourceFields) {
    auto model = make_model_with_args(R"({"duration":123,"name":"test"})");
    std::string file;
    int line = -1;
    EXPECT_FALSE(extract_source_location(model, model.events_[0], file, line));
}

TEST(ExtractSourceLocation, NoArgs) {
    TraceModel model;
    model.intern_string("");
    TraceEvent ev{};
    ev.args_idx = UINT32_MAX;
    model.events_.push_back(ev);
    std::string file;
    int line = -1;
    EXPECT_FALSE(extract_source_location(model, model.events_[0], file, line));
}

TEST(ExtractSourceLocation, LineAsString) {
    auto model = make_model_with_args(R"({"file":"test.py","line":"55"})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events_[0], file, line));
    EXPECT_EQ(file, "test.py");
    EXPECT_EQ(line, 55);
}
