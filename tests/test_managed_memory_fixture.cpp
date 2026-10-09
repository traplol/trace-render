#include <gtest/gtest.h>
#include "parser/diagsession_import.h"
#include "parser/managed_snapshot.h"
#include "parser/profile_io.h"
#include <tinyxml2.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Missing fixture: " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
}  // namespace

TEST(ManagedMemoryFixture, PerfViewGraphsMatchEveryTypeAgainstIndependentXmlOracle) {
    const char* directory = std::getenv("TRACE_GCDUMP_FIXTURES");
    if (!directory) GTEST_SKIP() << "Set TRACE_GCDUMP_FIXTURES to the downloaded official PerfView fixtures";
    for (const auto* name : {"test1", "test2"}) {
        SCOPED_TRACE(name);
        const std::filesystem::path root(directory);
        const auto data = read_fixture(root / (std::string(name) + ".gcdump"));
        const auto xml_bytes = read_fixture(root / (std::string(name) + "_baseline.gcdump.xml"));
        tinyxml2::XMLDocument xml;
        ASSERT_EQ(xml.Parse(xml_bytes.data(), xml_bytes.size()), tinyxml2::XML_SUCCESS);
        const auto* graph = xml.RootElement()->FirstChildElement("MemoryGraph");
        ASSERT_NE(graph, nullptr);
        ManagedSnapshot snapshot;
        std::string error;
        ASSERT_TRUE(read_gcdump(data, snapshot, error)) << error;
        ASSERT_EQ(snapshot.types.size(), graph->UnsignedAttribute("NumTypes"));
        const auto* type = graph->FirstChildElement("NodeTypes")->FirstChildElement("NodeType");
        for (; type; type = type->NextSiblingElement("NodeType"))
            EXPECT_EQ(snapshot.types.at(type->UnsignedAttribute("Index")).name, type->Attribute("Name"));
        std::vector<uint64_t> counts(snapshot.types.size()), sizes(snapshot.types.size());
        uint64_t count = 0, bytes = 0, groups = 0;
        const auto* node = graph->FirstChildElement("Nodes")->FirstChildElement("Node");
        for (; node; node = node->NextSiblingElement("Node")) {
            const auto index = node->UnsignedAttribute("TypeIndex");
            const auto size = node->Unsigned64Attribute("Size");
            const auto address = std::stoull(node->Attribute("Address"), nullptr, 16);
            const auto& name = snapshot.types.at(index).name;
            if (!address && !size && name.size() >= 2 && name.front() == '[' && name.back() == ']') {
                ++groups;
                continue;
            }
            ++counts.at(index);
            sizes.at(index) += size;
            ++count;
            bytes += size;
        }
        EXPECT_EQ(count + groups, graph->Unsigned64Attribute("NumNodes"));
        EXPECT_EQ(bytes, graph->Unsigned64Attribute("TotalSize"));
        EXPECT_EQ(snapshot.object_count, count);
        EXPECT_EQ(snapshot.live_bytes, bytes);
        EXPECT_FALSE(snapshot.sampled);
        for (size_t i = 0; i < counts.size(); ++i) {
            EXPECT_EQ(snapshot.types[i].object_count, counts[i]) << i;
            EXPECT_EQ(snapshot.types[i].size_bytes, sizes[i]) << i;
        }
    }
}

TEST(ManagedMemoryFixture, ProductionImportOpensAllNineMauiSnapshotsWithoutAllocationOrigins) {
    const char* path = std::getenv("TRACE_MANAGED_MEMORY_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_MANAGED_MEMORY_FIXTURE to memory-maui-nine-snapshots.diagsession";
    const auto bytes = read_fixture(path);
    ASSERT_EQ(bytes.size(), 38880293u);
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(bytes, model, error)) << error;
    const auto& profile = model.profile();
    EXPECT_TRUE(profile.capabilities.managed_heap_snapshots);
    EXPECT_FALSE(profile.capabilities.managed_allocation_history);
    EXPECT_FALSE(profile.capabilities.managed_survival);
    EXPECT_TRUE(profile.allocations.empty());
    EXPECT_TRUE(profile.managed_survival.empty());
    ASSERT_EQ(profile.managed_snapshots.size(), 9u);
    const std::array<uint64_t, 9> counts = {214168, 309675, 454135, 590429, 737780, 873091, 994330, 1114689, 1118691};
    const std::array<uint64_t, 9> sizes = {14042936, 19946112, 28836104, 36852568, 47159576,
                                           54796800, 61639056, 68209536, 68381464};
    const std::array<uint64_t, 9> times = {8976398100,  21442781100, 35625465900,  47899037800, 62880289000,
                                           78848665900, 96693644700, 120599375500, 157322484500};
    for (size_t i = 0; i < counts.size(); ++i) {
        const auto& snapshot = profile.managed_snapshots[i];
        EXPECT_EQ(snapshot.object_count, counts[i]);
        EXPECT_EQ(snapshot.live_bytes, sizes[i]);
        EXPECT_DOUBLE_EQ(snapshot.ts, times[i] / 1000.0);
        EXPECT_FALSE(snapshot.sampled);
        uint64_t count = 0, size = 0;
        for (const auto& type : snapshot.types) {
            count += type.object_count;
            size += type.size_bytes;
        }
        EXPECT_EQ(count, counts[i]);
        EXPECT_EQ(size, sizes[i]);
    }
    std::string saved;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    TraceModel reopened;
    ASSERT_TRUE(read_profile(saved, reopened, error)) << error;
    std::string saved_again;
    ASSERT_TRUE(serialize_profile(reopened, saved_again, error)) << error;
    EXPECT_EQ(saved_again, saved);
}
