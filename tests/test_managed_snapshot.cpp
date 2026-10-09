#include <gtest/gtest.h>
#include "parser/managed_snapshot.h"
#include "parser/diagsession_import.h"
#include "parser/profile_io.h"
#include <miniz.h>
#include <cstring>
#include <nlohmann/json.hpp>

namespace {
void integer(std::string& bytes, uint64_t value, size_t width = 4) {
    for (size_t i = 0; i < width; ++i) bytes += char(value >> (i * 8));
}
void text(std::string& bytes, const std::string& value) {
    integer(bytes, value.size());
    bytes += value;
}
void object(std::string& bytes, const std::string& name, uint32_t version, uint32_t minimum) {
    bytes += "\x04\x04\x01";
    integer(bytes, version);
    integer(bytes, minimum);
    text(bytes, name);
    bytes += '\x06';
}
void weight(std::string& bytes, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    integer(bytes, bits);
}

std::string snapshot_bytes(bool sampled = false, bool unknown = false) {
    std::string bytes;
    text(bytes, "!FastSerialization.1");
    object(bytes, "GCHeapDump", 10, 8);
    object(bytes, "Graphs.MemoryGraph", 1, 0);
    integer(bytes, 48, 8);
    integer(bytes, 0);  // root
    integer(bytes, 3);  // types
    for (const auto& name : {"UNDEFINED", "[ROOT]", "RetainedPayload"}) {
        text(bytes, name);
        integer(bytes, 0);
        integer(bytes, UINT32_MAX);  // no module
    }
    integer(bytes, 3);  // nodes
    integer(bytes, 0);
    integer(bytes, 4);
    integer(bytes, 8);
    // Root has two children. One sized object points backward to the root.
    std::string nodes("\x02\x02\x01\x02\x05\x10\x01\x7f\x05\x20\x00", 11);
    integer(bytes, nodes.size());
    bytes += nodes;
    integer(bytes, 3);
    integer(bytes, 0, 8);
    integer(bytes, unknown ? 0 : 0x1000, 8);
    integer(bytes, 0x2000, 8);
    bytes += std::string("\x08\x01\x06\x01", 4);  // graph bitness/end, legacy bitness
    weight(bytes, sampled ? 2.0f : 1.0f);
    weight(bytes, sampled ? 3.0f : 1.0f);
    bytes += "\x01\x01";         // null JS and .NET heap metadata
    integer(bytes, UINT32_MAX);  // collection log
    integer(bytes, 0, 8);        // time
    integer(bytes, UINT32_MAX);  // machine
    integer(bytes, UINT32_MAX);  // process name
    integer(bytes, 7);
    integer(bytes, 0, 8);
    integer(bytes, 0, 8);
    integer(bytes, sampled ? 3 : 0);
    if (sampled) {
        weight(bytes, 1);
        weight(bytes, 1);
        weight(bytes, 4);
    }
    bytes += "\x06\x06";  // dump end and stream object terminator
    const auto references = bytes.size();
    integer(bytes, 0);
    const auto trailer = bytes.size();
    integer(bytes, references);
    integer(bytes, trailer);
    return bytes;
}

TraceModel snapshot_model(const ManagedSnapshot& snapshot) {
    TraceModel model;
    model.intern_string("");
    ProfileData profile;
    profile.capabilities.managed_heap_snapshots = true;
    profile.processes.push_back({"process", 7, {}, {}});
    profile.managed_snapshots.push_back(snapshot);
    profile.managed_snapshots[0].id = "snapshot";
    profile.managed_snapshots[0].process_id = "process";
    profile.managed_snapshots[0].ts = 100;
    model.set_profile(std::move(profile));
    model.build_index();
    return model;
}
}  // namespace

TEST(ManagedSnapshot, ExcludesOnlySyntheticGroupsAndPreservesRawSampledCounts) {
    ManagedSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(read_gcdump(snapshot_bytes(true), snapshot, error)) << error;
    EXPECT_EQ(snapshot.object_count, 2u);
    EXPECT_EQ(snapshot.live_bytes, 48u);
    ASSERT_EQ(snapshot.types.size(), 3u);
    EXPECT_EQ(snapshot.types[1].object_count, 0u);
    EXPECT_EQ(snapshot.types[2].object_count, 2u);
    EXPECT_EQ(snapshot.types[2].size_bytes, 48u);
    EXPECT_EQ(snapshot.types[2].count_multiplier, 4);
    EXPECT_EQ(snapshot.average_count_multiplier, 2);
    EXPECT_EQ(snapshot.average_size_multiplier, 3);
    EXPECT_TRUE(snapshot.sampled);
    EXPECT_FALSE(snapshot.incomplete);
    ASSERT_TRUE(read_gcdump(snapshot_bytes(false, true), snapshot, error)) << error;
    EXPECT_EQ(snapshot.object_count, 2u);
    EXPECT_TRUE(snapshot.incomplete);
    EXPECT_FALSE(snapshot.warnings.empty());
}

TEST(ManagedSnapshot, RejectsTruncationInvalidNodeOffsetsAndUnknownGraphVersions) {
    auto bytes = snapshot_bytes();
    ManagedSnapshot snapshot;
    std::string error;
    for (size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_FALSE(read_gcdump(std::string_view(bytes).substr(0, size), snapshot, error)) << size;
        EXPECT_TRUE(snapshot.types.empty());
    }
    auto bad = bytes;
    // A node definition starts at an offset beyond the blob.
    const auto graph = bad.find("Graphs.MemoryGraph") + 19;
    const auto nodes = bad.find(std::string("\x03\0\0\0\0\0\0\0\x04\0\0\0\x08", 13), graph);
    ASSERT_NE(nodes, std::string::npos);
    bad[nodes + 4] = '\x7f';
    EXPECT_FALSE(read_gcdump(bad, snapshot, error));
    EXPECT_NE(error.find("node"), std::string::npos);
    bad = bytes;
    bad[bad.find("Graphs.MemoryGraph") - 12] = 2;
    EXPECT_FALSE(read_gcdump(bad, snapshot, error));
    EXPECT_NE(error.find("version"), std::string::npos);
    EXPECT_FALSE(read_gcdump(bytes, snapshot, error, [](const char*, float) { return false; }));
    EXPECT_NE(error.find("canceled"), std::string::npos);
}

TEST(ManagedSnapshot, ProfileReopenPreservesSnapshotQualityAndRejectsInvalidMultipliers) {
    ManagedSnapshot snapshot;
    std::string error, bytes;
    ASSERT_TRUE(read_gcdump(snapshot_bytes(true, true), snapshot, error)) << error;
    auto model = snapshot_model(snapshot);
    ASSERT_TRUE(serialize_profile(model, bytes, error)) << error;
    TraceModel restored;
    ASSERT_TRUE(read_profile(bytes, restored, error)) << error;
    const auto& saved = restored.profile().managed_snapshots.at(0);
    EXPECT_TRUE(saved.sampled);
    EXPECT_TRUE(saved.incomplete);
    EXPECT_EQ(saved.warnings, snapshot.warnings);
    EXPECT_EQ(saved.types[2].count_multiplier, 4);
    EXPECT_EQ(saved.average_size_multiplier, 3);
    EXPECT_TRUE(restored.profile().allocations.empty());
    EXPECT_FALSE(restored.profile().quality.sampled_allocations);
    auto json = nlohmann::json::parse(bytes.substr(PROFILE_MAGIC.size()));
    json["profile"]["managed_snapshots"][0]["types"][2]["count_multiplier"] = -1;
    EXPECT_FALSE(read_profile(std::string(PROFILE_MAGIC) + json.dump(), restored, error));
    EXPECT_NE(error.find("positive"), std::string::npos);
}

TEST(ManagedSnapshot, SnapshotOnlyContainerKeepsUsableDataWhenAnotherSnapshotIsUnsupported) {
    const std::string manifest = R"({"Version":1,"IsManagedEnabled":true,"Snapshots":[
      {"SnapshotTime":10000,"ProcessId":7,"Heaps":[{"Type":"PROFILER_MANAGED","GCDumpFileName":"good.gcdump"}]},
      {"SnapshotTime":20000,"ProcessId":7,"Heaps":[{"Type":"PROFILER_MANAGED","GCDumpFileName":"bad.gcdump"}]}]})";
    const std::string metadata = R"(<Package><Content>
      <Resource Id="manifest" Type="MemoryProfiler.Manifest" Name="manifest.json" ResourcePackageUriPrefix="manifest" IsDirectoryOnDisk="false"/>
      <Resource Id="good" Type="MemoryProfiler.GCDump" Name="good.gcdump" ResourcePackageUriPrefix="good" IsDirectoryOnDisk="false"/>
      <Resource Id="bad" Type="MemoryProfiler.GCDump" Name="bad.gcdump" ResourcePackageUriPrefix="bad" IsDirectoryOnDisk="false"/>
      </Content></Package>)";
    mz_zip_archive zip{};
    ASSERT_TRUE(mz_zip_writer_init_heap(&zip, 0, 0));
    const auto snapshot = snapshot_bytes();
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "metadata.xml", metadata.data(), metadata.size(), MZ_BEST_SPEED));
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "manifest/manifest.json", manifest.data(), manifest.size(), MZ_BEST_SPEED));
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "good/good.gcdump", snapshot.data(), snapshot.size(), MZ_BEST_SPEED));
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "bad/bad.gcdump", "unsupported", 11, MZ_BEST_SPEED));
    void* data = nullptr;
    size_t size = 0;
    ASSERT_TRUE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    const std::string capture(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_writer_end(&zip);
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(capture, model, error)) << error;
    ASSERT_EQ(model.profile().managed_snapshots.size(), 1u);
    EXPECT_EQ(model.profile().managed_snapshots[0].object_count, 2u);
    EXPECT_EQ(model.profile().managed_snapshots[0].ts, 10);
    EXPECT_TRUE(model.profile().quality.incomplete_capture);
    EXPECT_FALSE(model.profile().quality.warnings.empty());
    EXPECT_TRUE(model.profile().allocations.empty());
    EXPECT_FALSE(model.profile().capabilities.managed_allocation_history);
    EXPECT_FALSE(read_diagsession(capture, model, error, [](const char*, float) { return false; }));
    EXPECT_NE(error.find("canceled"), std::string::npos);
}
