#include <gtest/gtest.h>
#include "parser/diagsession_container.h"
#include "parser/diagsession_import.h"
#include "parser/profile_io.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace {
// Independent reader for the original VS 17.8 x64 heapstate v2 fixture.
// The four observed tables are documented in docs/diagsession-corpus.md.
uint64_t snapshot_integer(const std::vector<uint8_t>& data, size_t offset, size_t width = 8) {
    if (offset > data.size() || width > data.size() - offset) throw std::runtime_error("Truncated fixture heapstate");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(data[offset + i]) << (8 * i);
    return value;
}

double fixture_timestamp(uint64_t qpc) {
    // Independently inspected ETL origin and QPC frequency, fixed for this fixture.
    return static_cast<double>(qpc - UINT64_C(883560081719)) / 10;
}
}  // namespace

TEST(NativeMemoryFixture, ProductionImportMatchesEverySavedSnapshotAllocationAndStack) {
    const char* path = std::getenv("TRACE_NATIVE_MEMORY_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_NATIVE_MEMORY_FIXTURE to native-memory-cscn.diagsession";
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input) << path;
    std::string capture_bytes(std::istreambuf_iterator<char>(input), {});
    ASSERT_EQ(capture_bytes.size(), 49023829u);
    TraceModel model;
    std::string error;
    // Check the fixed clock anchor directly in the fixture's raw logfile header.
    DiagsessionContainer container;
    ASSERT_TRUE(container.open(capture_bytes, error)) << error;
    bool found_etl = false;
    for (size_t i = 0; i < container.resources().size(); ++i) {
        if (container.resources()[i].type != "DiagnosticsHub.Resource.EtlFile") continue;
        std::vector<uint8_t> etl;
        ASSERT_TRUE(container.read_resource(i, etl, error)) << error;
        ASSERT_EQ(snapshot_integer(etl, 88), 883560081719u);
        ASSERT_EQ(snapshot_integer(etl, 360), 10000000u);
        found_etl = true;
    }
    ASSERT_TRUE(found_etl);
    ASSERT_TRUE(read_diagsession(capture_bytes, model, error)) << error;
    ASSERT_TRUE(model.profile().capabilities.native_allocation_history);
    ASSERT_EQ(model.profile().allocations.size(), 311635u);
    ASSERT_TRUE(model.profile().capture_end_ts);
    EXPECT_EQ(model.profile().quality.lost_events, 0u);
    EXPECT_FALSE(model.profile().quality.allocation_history_gaps);
    size_t moved_realloc_allocations = 0;
    for (const auto& allocation : model.profile().allocations) {
        ASSERT_TRUE(allocation.allocated_ts);
        EXPECT_NE(*allocation.allocated_ts, fixture_timestamp(883562417100));
        if (*allocation.allocated_ts == fixture_timestamp(883562417062)) {
            ++moved_realloc_allocations;
            EXPECT_EQ(allocation.size_bytes, 564u);
        }
    }
    EXPECT_EQ(moved_realloc_allocations, 1u);
    std::string process_id;
    for (const auto& process : model.profile().processes) {
        if (process.pid == 25260) {
            ASSERT_TRUE(process_id.empty());
            process_id = process.id;
        }
    }
    ASSERT_FALSE(process_id.empty());
    std::unordered_map<std::string, int32_t> frames_by_id;
    for (size_t i = 0; i < model.stack_frames().size(); ++i)
        frames_by_id.emplace(model.get_string(model.stack_frames()[i].id_idx), static_cast<int32_t>(i));

    const std::array<uint64_t, 6> expected_qpc = {883681963340, 884799041469, 887346737139,
                                                  888414448792, 889601553015, 889970253814};
    const std::array<uint64_t, 6> expected_counts = {276, 4504, 4512, 4502, 4502, 4502};
    const std::array<uint64_t, 6> expected_bytes = {86779, 1069378, 1072650, 1068330, 1068330, 1068330};
    size_t checked_allocations = 0;
    for (size_t snapshot = 0; snapshot < 6; ++snapshot) {
        const std::string name = "snapshot" + std::to_string(snapshot) + ".heapstate";
        SCOPED_TRACE(name);
        std::vector<uint8_t> data;
        for (size_t i = 0; i < container.resources().size(); ++i) {
            if (container.resources()[i].name == name) ASSERT_TRUE(container.read_resource(i, data, error)) << error;
        }
        ASSERT_FALSE(data.empty());
        ASSERT_EQ(snapshot_integer(data, 0, 4), 0x407208f6u);
        ASSERT_EQ(snapshot_integer(data, 4, 4), 2u);
        ASSERT_EQ(snapshot_integer(data, 8, 4), 8u);
        ASSERT_EQ(snapshot_integer(data, 12, 4), 0u);
        ASSERT_EQ(snapshot_integer(data, 52, 4), 4u);
        ASSERT_EQ(snapshot_integer(data, 32), expected_qpc[snapshot]);
        std::array<size_t, 4> table_offsets{}, table_counts{};
        const std::array<size_t, 4> widths = {24, 32, 32, 8};
        size_t position = 0x240;
        for (size_t table = 0; table < 4; ++table) {
            ASSERT_EQ(snapshot_integer(data, position + 16, 4), 1u);
            ASSERT_EQ(snapshot_integer(data, position + 20, 4), widths[table]);
            table_counts[table] = snapshot_integer(data, position + 24, 4);
            position += 28;
            ASSERT_LE(position, data.size());
            ASSERT_LE(table_counts[table], (data.size() - position) / widths[table]);
            table_offsets[table] = position;
            position += widths[table] * table_counts[table];
        }
        ASSERT_EQ(position, data.size());
        ASSERT_EQ(table_counts[0], 1u);
        ASSERT_EQ(table_counts[1], expected_counts[snapshot]);
        EXPECT_EQ(snapshot_integer(data, table_offsets[0] + 8), expected_bytes[snapshot]);
        EXPECT_EQ(snapshot_integer(data, table_offsets[0] + 20, 4), expected_counts[snapshot]);

        const double ts = fixture_timestamp(expected_qpc[snapshot]);
        const double cohort_start = snapshot ? fixture_timestamp(expected_qpc[snapshot - 1]) : 0;
        const auto result = model.query_outstanding_memory(ts, std::nullopt, process_id);
        ASSERT_TRUE(result.error.empty()) << result.error;
        EXPECT_EQ(result.total.known.count, expected_counts[snapshot]);
        EXPECT_EQ(result.total.known.bytes, expected_bytes[snapshot]);
        EXPECT_EQ(result.total.uncertain.count, 0u);
        std::map<uint64_t, const AllocationLifetime*> live;
        for (const auto& allocation : model.profile().allocations) {
            if (allocation.process_id == process_id && allocation.allocated_ts && *allocation.allocated_ts <= ts &&
                (!allocation.freed_ts || *allocation.freed_ts > ts)) {
                ASSERT_TRUE(allocation.address);
                ASSERT_TRUE(live.emplace(*allocation.address, &allocation).second);
            }
        }
        ASSERT_EQ(live.size(), expected_counts[snapshot]);
        uint64_t cohort_bytes = 0, cohort_count = 0, snapshot_bytes = 0;
        std::map<int32_t, AllocationAmount> expected_stacks;
        for (size_t row = 0; row < table_counts[1]; ++row) {
            const size_t at = table_offsets[1] + row * 32;
            const uint64_t birth_qpc = snapshot_integer(data, at);
            const uint64_t size = snapshot_integer(data, at + 8);
            const uint64_t address = snapshot_integer(data, at + 16);
            const uint64_t stack = snapshot_integer(data, at + 24);
            ASSERT_LT(stack, table_counts[2]);
            auto found = live.find(address);
            ASSERT_NE(found, live.end()) << address;
            const auto& allocation = *found->second;
            EXPECT_EQ(allocation.size_bytes, size);
            ASSERT_TRUE(allocation.allocated_ts);
            ASSERT_DOUBLE_EQ(*allocation.allocated_ts, fixture_timestamp(birth_qpc));
            auto leaf = frames_by_id.find(allocation.stack_frame_id);
            ASSERT_NE(leaf, frames_by_id.end());
            auto& amount = expected_stacks[leaf->second];
            amount.bytes += size;
            ++amount.count;
            std::vector<uint64_t> actual_stack;
            for (int32_t frame = leaf->second; frame >= 0; frame = model.stack_frames()[frame].parent_idx) {
                ASSERT_TRUE(model.stack_frames()[frame].address);
                actual_stack.push_back(*model.stack_frames()[frame].address);
            }
            const size_t stack_at = table_offsets[2] + stack * 32;
            const uint64_t start = snapshot_integer(data, stack_at, 4);
            const uint64_t depth = snapshot_integer(data, stack_at + 4, 4);
            ASSERT_LE(start + depth, table_counts[3]);
            // These producer snapshots omit exactly the final ETL root frame.
            ASSERT_EQ(actual_stack.size(), depth + 1);
            EXPECT_TRUE(actual_stack.back() == 0x7ffabd102651 || actual_stack.back() == 0x7ffabd124b0e);
            for (size_t frame = 0; frame < depth; ++frame)
                EXPECT_EQ(actual_stack[frame], snapshot_integer(data, table_offsets[3] + (start + frame) * 8));
            snapshot_bytes += size;
            if (fixture_timestamp(birth_qpc) >= cohort_start && fixture_timestamp(birth_qpc) < ts) {
                cohort_bytes += size;
                ++cohort_count;
            }
            live.erase(found);
            ++checked_allocations;
        }
        EXPECT_TRUE(live.empty());
        EXPECT_EQ(snapshot_bytes, expected_bytes[snapshot]);
        ASSERT_EQ(result.stacks.size(), expected_stacks.size());
        for (const auto& stack : result.stacks) {
            EXPECT_EQ(stack.process_id, process_id);
            const auto expected = expected_stacks.find(stack.frame_index);
            ASSERT_NE(expected, expected_stacks.end());
            EXPECT_EQ(stack.total.known.count, expected->second.count);
            EXPECT_EQ(stack.total.known.bytes, expected->second.bytes);
        }
        const auto cohort = model.query_outstanding_memory(ts, {{cohort_start, ts}}, process_id);
        ASSERT_TRUE(cohort.error.empty()) << cohort.error;
        EXPECT_EQ(cohort.total.known.count, cohort_count);
        EXPECT_EQ(cohort.total.known.bytes, cohort_bytes);
        EXPECT_EQ(cohort.total.uncertain.count, 0u);
    }
    EXPECT_EQ(checked_allocations, 22798u);
    const auto final = model.query_outstanding_memory(*model.profile().capture_end_ts, std::nullopt, process_id);
    EXPECT_EQ(final.total.known.count, 4502u);
    EXPECT_EQ(final.total.known.bytes, 1068330u);
    EXPECT_EQ(final.total.uncertain.count, 0u);

    // A saved profile must answer the same queries without its original capture.
    std::string saved;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    model.clear();
    ASSERT_TRUE(read_profile(saved, model, error)) << error;
    for (size_t i = 0; i < expected_qpc.size(); ++i) {
        const auto restored =
            model.query_outstanding_memory(fixture_timestamp(expected_qpc[i]), std::nullopt, process_id);
        EXPECT_EQ(restored.total.known.count, expected_counts[i]);
        EXPECT_EQ(restored.total.known.bytes, expected_bytes[i]);
        EXPECT_EQ(restored.total.uncertain.count, 0u);
    }
}
