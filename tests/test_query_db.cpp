#include <gtest/gtest.h>
#include "model/query_db.h"
#include "parser/trace_parser.h"

class QueryDbTest : public ::testing::Test {
protected:
    TraceModel model;
    QueryDb db;

    void SetUp() override {
        TraceParser parser;
        ASSERT_TRUE(parser.parse("test_trace.json", model));
        db.load(model);
    }
};

TEST_F(QueryDbTest, IsLoaded) {
    EXPECT_TRUE(db.is_loaded());
}

TEST_F(QueryDbTest, SelectAllEvents) {
    auto result = db.execute("SELECT COUNT(*) FROM events");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.rows.size(), 1u);
    int count = std::stoi(result.rows[0][0]);
    EXPECT_GT(count, 0);
}

TEST_F(QueryDbTest, SelectProcesses) {
    auto result = db.execute("SELECT name FROM processes ORDER BY pid");
    ASSERT_TRUE(result.ok);
    ASSERT_GE(result.rows.size(), 2u);
    EXPECT_EQ(result.rows[0][0], "Browser");
    EXPECT_EQ(result.rows[1][0], "Renderer");
}

TEST_F(QueryDbTest, SelectThreads) {
    auto result = db.execute("SELECT name FROM threads WHERE pid=1 AND tid=1");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][0], "CrBrowserMain");
}

TEST_F(QueryDbTest, AggregateByName) {
    auto result = db.execute(
        "SELECT name, COUNT(*) as cnt, SUM(dur) as total "
        "FROM events WHERE dur > 0 GROUP BY name ORDER BY total DESC");
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.rows.empty());
    ASSERT_EQ(result.columns.size(), 3u);
    EXPECT_EQ(result.columns[0], "name");
    EXPECT_EQ(result.columns[1], "cnt");
    EXPECT_EQ(result.columns[2], "total");
}

TEST_F(QueryDbTest, SelectCounters) {
    auto result = db.execute("SELECT COUNT(*) FROM counters WHERE name='FrameTime'");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.rows.size(), 1u);
    int count = std::stoi(result.rows[0][0]);
    EXPECT_GT(count, 0);
}

TEST_F(QueryDbTest, JoinEventsWithProcesses) {
    auto result = db.execute(
        "SELECT e.name, p.name as proc_name "
        "FROM events e JOIN processes p ON e.pid = p.pid "
        "WHERE e.name = 'HTMLParser'");
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0][1], "Renderer");
}

TEST_F(QueryDbTest, InvalidSqlReturnsError) {
    auto result = db.execute("SELEKT bad syntax");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(QueryDbTest, EmptyDbReturnsError) {
    QueryDb empty_db;
    auto result = empty_db.execute("SELECT 1");
    EXPECT_FALSE(result.ok);
}
