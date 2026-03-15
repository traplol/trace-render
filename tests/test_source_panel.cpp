#include <gtest/gtest.h>
#include "ui/source_panel.h"

// --- extract_source_location tests ---

static TraceModel make_model_with_args(const std::string& args_json) {
    TraceModel model;
    model.intern_string("");  // idx 0
    model.add_args(args_json);
    TraceEvent ev{};
    ev.args_idx = 0;
    model.add_event(ev);
    return model;
}

TEST(ExtractSourceLocation, FileAndLine) {
    auto model = make_model_with_args(R"({"file":"src/main.cpp","line":42})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events()[0], file, line));
    EXPECT_EQ(file, "src/main.cpp");
    EXPECT_EQ(line, 42);
}

TEST(ExtractSourceLocation, SrcFileSrcLine) {
    auto model = make_model_with_args(R"({"src_file":"foo.h","src_line":10})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events()[0], file, line));
    EXPECT_EQ(file, "foo.h");
    EXPECT_EQ(line, 10);
}

TEST(ExtractSourceLocation, FileNameLineNumber) {
    auto model = make_model_with_args(R"({"fileName":"bar.cc","lineNumber":99})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events()[0], file, line));
    EXPECT_EQ(file, "bar.cc");
    EXPECT_EQ(line, 99);
}

TEST(ExtractSourceLocation, FileOnly) {
    auto model = make_model_with_args(R"({"file":"only_file.cpp"})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events()[0], file, line));
    EXPECT_EQ(file, "only_file.cpp");
    EXPECT_EQ(line, -1);
}

TEST(ExtractSourceLocation, NoSourceFields) {
    auto model = make_model_with_args(R"({"duration":123,"name":"test"})");
    std::string file;
    int line = -1;
    EXPECT_FALSE(extract_source_location(model, model.events()[0], file, line));
}

TEST(ExtractSourceLocation, NoArgs) {
    TraceModel model;
    model.intern_string("");
    TraceEvent ev{};
    ev.args_idx = UINT32_MAX;
    model.add_event(ev);
    std::string file;
    int line = -1;
    EXPECT_FALSE(extract_source_location(model, model.events()[0], file, line));
}

TEST(ExtractSourceLocation, LineAsString) {
    auto model = make_model_with_args(R"({"file":"test.py","line":"55"})");
    std::string file;
    int line = -1;
    EXPECT_TRUE(extract_source_location(model, model.events()[0], file, line));
    EXPECT_EQ(file, "test.py");
    EXPECT_EQ(line, 55);
}

// --- remap_source_path tests ---

TEST(RemapSourcePath, NoRemapping) {
    EXPECT_EQ(remap_source_path("/home/user/file.cpp", "", ""), "/home/user/file.cpp");
}

TEST(RemapSourcePath, BackslashNormalization) {
    EXPECT_EQ(remap_source_path("repo\\lib\\file.cpp", "", ""), "repo/lib/file.cpp");
}

TEST(RemapSourcePath, LocalBaseOnly) {
    // Relative path + local base = prepend
    EXPECT_EQ(remap_source_path("repo\\lib\\file.cpp", "", "/mnt/c/dev"), "/mnt/c/dev/repo/lib/file.cpp");
}

TEST(RemapSourcePath, WindowsDriveToWSL) {
    // Strip "c:\dev\repo" and replace with "/mnt/c/dev/repo"
    EXPECT_EQ(remap_source_path("c:\\dev\\repo\\lib\\file.cpp", "c:\\dev\\repo", "/mnt/c/dev/repo"),
              "/mnt/c/dev/repo/lib/file.cpp");
}

TEST(RemapSourcePath, JenkinsProdToLocalWSL) {
    // Strip jenkins prod path, replace with local WSL path
    EXPECT_EQ(remap_source_path("c:\\jenkins\\prod\\rel-123\\repo\\lib\\file.cpp", "c:\\jenkins\\prod\\rel-123\\repo",
                                "/mnt/c/dev/repo"),
              "/mnt/c/dev/repo/lib/file.cpp");
}

TEST(RemapSourcePath, JenkinsProdToLocalWindows) {
    // Strip jenkins prod path, replace with local Windows path
    EXPECT_EQ(
        remap_source_path("c:\\jenkins\\prod\\rel-123\\repo\\lib\\file.cpp", "c:\\jenkins\\prod\\rel-123", "c:\\dev"),
        "c:/dev/repo/lib/file.cpp");
}

TEST(RemapSourcePath, CaseInsensitiveStrip) {
    // Windows paths are case-insensitive
    EXPECT_EQ(remap_source_path("C:\\Dev\\Repo\\lib\\file.cpp", "c:\\dev\\repo", "/mnt/c/dev/repo"),
              "/mnt/c/dev/repo/lib/file.cpp");
}

TEST(RemapSourcePath, StripPrefixNoMatch) {
    // When strip prefix doesn't match, path is unchanged (except normalization) + base prepended
    EXPECT_EQ(remap_source_path("d:\\other\\file.cpp", "c:\\dev", "/mnt/c/dev"), "/mnt/c/dev/d:/other/file.cpp");
}

TEST(RemapSourcePath, StripOnly) {
    // Strip without adding a base
    EXPECT_EQ(remap_source_path("c:\\dev\\repo\\lib\\file.cpp", "c:\\dev\\repo", ""), "/lib/file.cpp");
}

TEST(RemapSourcePath, TrailingSlashesInStrip) {
    // Trailing slashes in strip prefix should be handled
    EXPECT_EQ(remap_source_path("c:\\dev\\repo\\file.cpp", "c:\\dev\\repo\\", "/mnt/c/dev/repo"),
              "/mnt/c/dev/repo/file.cpp");
}

TEST(RemapSourcePath, TrailingSlashesInBase) {
    // Trailing slashes in base should not double up
    EXPECT_EQ(remap_source_path("lib/file.cpp", "", "/mnt/c/dev/"), "/mnt/c/dev/lib/file.cpp");
}

TEST(RemapSourcePath, AbsolutePathWithBase) {
    // Absolute path with base but no strip — base is still prepended
    EXPECT_EQ(remap_source_path("/usr/src/file.cpp", "", "/home/user"), "/home/user/usr/src/file.cpp");
}
