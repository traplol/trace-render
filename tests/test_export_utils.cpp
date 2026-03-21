#include <gtest/gtest.h>
#include "ui/export_utils.h"

static QueryDb::QueryResult make_result(std::vector<std::string> cols, std::vector<std::vector<std::string>> rows) {
    QueryDb::QueryResult r;
    r.columns = std::move(cols);
    r.rows = std::move(rows);
    r.ok = true;
    return r;
}

TEST(ExportUtils, CsvBasic) {
    auto r = make_result({"name", "count"}, {{"foo", "3"}, {"bar", "7"}});
    std::string csv = export_result(r, ',');
    EXPECT_EQ(csv, "name,count\nfoo,3\nbar,7\n");
}

TEST(ExportUtils, TsvBasic) {
    auto r = make_result({"name", "count"}, {{"foo", "3"}, {"bar", "7"}});
    std::string tsv = export_result(r, '\t');
    EXPECT_EQ(tsv, "name\tcount\nfoo\t3\nbar\t7\n");
}

TEST(ExportUtils, CsvQuotesFieldsWithCommas) {
    auto r = make_result({"val"}, {{"hello, world"}, {"plain"}});
    std::string csv = export_result(r, ',');
    EXPECT_EQ(csv, "val\n\"hello, world\"\nplain\n");
}

TEST(ExportUtils, CsvQuotesFieldsWithQuotes) {
    auto r = make_result({"val"}, {{"say \"hi\""}});
    std::string csv = export_result(r, ',');
    EXPECT_EQ(csv, "val\n\"say \"\"hi\"\"\"\n");
}

TEST(ExportUtils, CsvQuotesFieldsWithNewlines) {
    auto r = make_result({"val"}, {{"line1\nline2"}});
    std::string csv = export_result(r, ',');
    EXPECT_EQ(csv, "val\n\"line1\nline2\"\n");
}

TEST(ExportUtils, TsvNoQuoting) {
    // TSV should not quote fields even if they contain commas or quotes
    auto r = make_result({"val"}, {{"hello, world"}, {"say \"hi\""}});
    std::string tsv = export_result(r, '\t');
    EXPECT_EQ(tsv, "val\nhello, world\nsay \"hi\"\n");
}

TEST(ExportUtils, EmptyResult) {
    auto r = make_result({"a", "b"}, {});
    EXPECT_EQ(export_result(r, ','), "a,b\n");
    EXPECT_EQ(export_result(r, '\t'), "a\tb\n");
}

TEST(ExportUtils, SingleColumn) {
    auto r = make_result({"x"}, {{"1"}, {"2"}});
    EXPECT_EQ(export_result(r, ','), "x\n1\n2\n");
}
