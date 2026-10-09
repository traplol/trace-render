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

namespace {
// Hashes and independent Windows XML/physical ETL derivation are in docs/managed-allocations.md.
constexpr uint64_t desktop_origin = 12598809889;
constexpr uint64_t core_origin = 10965367735;
double at(uint64_t qpc, uint64_t origin = desktop_origin) {
    return static_cast<double>(qpc - origin) / 10;
}
std::string fixture_bytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("Missing managed fixture: ") + path);
    return {std::istreambuf_iterator<char>(input), {}};
}
void verify_clock(const std::string& bytes, uint64_t origin) {
    DiagsessionContainer container;
    std::string error;
    ASSERT_TRUE(container.open(bytes, error)) << error;
    size_t etls = 0;
    for (size_t i = 0; i < container.resources().size(); ++i) {
        if (container.resources()[i].type != "DiagnosticsHub.Resource.EtlFile") continue;
        ++etls;
        std::vector<uint8_t> data;
        ASSERT_TRUE(container.read_resource(i, data, error)) << error;
        ASSERT_GE(data.size(), 368u);
        // Read the raw logfile anchor directly, independently of importer timestamp conversion.
        auto integer = [&](size_t offset) {
            uint64_t value = 0;
            for (size_t j = 0; j < 8; ++j) value |= uint64_t(data[offset + j]) << (8 * j);
            return value;
        };
        EXPECT_EQ(integer(88), origin);
        EXPECT_EQ(integer(360), 10000000u);
    }
    EXPECT_EQ(etls, 1u);
}
std::string process_for(const TraceModel& model, uint32_t pid) {
    std::string result;
    for (const auto& process : model.profile().processes) {
        if (process.pid != pid) continue;
        EXPECT_TRUE(result.empty());
        result = process.id;
    }
    EXPECT_FALSE(result.empty());
    return result;
}
bool named(const TraceModel& model, int32_t frame, const std::string& prefix) {
    return frame >= 0 && model.get_string(model.stack_frames()[frame].name_idx).rfind(prefix, 0) == 0;
}
AllocationAmount origin_total(const TraceModel& model, const std::string& prefix,
                              std::optional<uint64_t> size = std::nullopt) {
    std::map<std::string, int32_t> frames;
    for (size_t i = 0; i < model.stack_frames().size(); ++i)
        frames.emplace(model.get_string(model.stack_frames()[i].id_idx), static_cast<int32_t>(i));
    AllocationAmount total;
    for (const auto& allocation : model.profile().allocations) {
        if (size && allocation.size_bytes != *size) continue;
        const auto leaf = frames.find(allocation.stack_frame_id);
        if (leaf == frames.end()) continue;
        for (auto frame = leaf->second; frame >= 0; frame = model.stack_frames()[frame].parent_idx) {
            if (!named(model, frame, prefix)) continue;
            ++total.count;
            total.bytes += allocation.size_bytes;
            break;
        }
    }
    return total;
}
void amount(const AllocationAmount& actual, uint64_t count, uint64_t bytes) {
    EXPECT_EQ(actual.count, count);
    EXPECT_EQ(actual.bytes, bytes);
}
void function_total(const TraceModel& model, const OutstandingMemory& query, const std::string& prefix, uint64_t count,
                    uint64_t bytes) {
    AllocationAmount total;
    size_t matches = 0;
    for (const auto& function : query.functions) {
        if (!named(model, function.frame_index, prefix)) continue;
        ++matches;
        total.count += function.inclusive.known.count;
        total.bytes += function.inclusive.known.bytes;
        EXPECT_EQ(function.inclusive.uncertain.count, 0u);
    }
    EXPECT_GT(matches, 0u) << prefix;
    amount(total, count, bytes);
}
void verify_history(const TraceModel& model, size_t count, uint64_t bytes) {
    const auto& p = model.profile();
    EXPECT_TRUE(p.capabilities.managed_allocation_history);
    ASSERT_EQ(p.allocations.size(), count);
    uint64_t actual_bytes = 0;
    for (const auto& a : p.allocations) {
        actual_bytes += a.size_bytes;
        EXPECT_EQ(a.kind, AllocationKind::Managed);
        EXPECT_TRUE(a.allocated_ts);
        EXPECT_FALSE(a.stack_frame_id.empty());
        EXPECT_FALSE(a.address);  // Profiler identities are not physical managed addresses.
        EXPECT_FALSE(a.freed_ts);
        EXPECT_EQ(a.end_state, AllocationEnd::Unknown);
    }
    EXPECT_EQ(actual_bytes, bytes);
    EXPECT_EQ(p.quality.lost_events, 0u);
    EXPECT_TRUE(p.quality.incomplete_capture);
    EXPECT_FALSE(p.quality.sampled_allocations);  // Sampling is unspecified, not known to be enabled.
    bool sampling_note = false;
    for (const auto& warning : p.quality.warnings) sampling_note |= warning.find("sampling") != std::string::npos;
    EXPECT_TRUE(sampling_note);
}
void round_trip(const TraceModel& model, TraceModel& reopened) {
    std::string saved, error, again;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    ASSERT_TRUE(read_profile(saved, reopened, error)) << error;
    ASSERT_TRUE(serialize_profile(reopened, again, error)) << error;
    EXPECT_EQ(again, saved);  // All names, IDs, observations, capabilities, and quality survive serialization.
}
void desktop_queries(const TraceModel& model) {
    const auto process = process_for(model, 4544);
    const std::array<uint64_t, 3> ends = {12633453567, 12633652060, 12633732427};
    const std::array<uint64_t, 3> counts = {5812, 5836, 5836};
    const std::array<uint64_t, 3> sizes = {8683904, 8816564, 8685532};
    for (size_t i = 0; i < ends.size(); ++i) {
        SCOPED_TRACE(ends[i]);
        auto query = model.query_outstanding_memory(at(ends[i]), {}, process, AllocationKind::Managed);
        ASSERT_TRUE(query.error.empty()) << query.error;
        amount(query.total.known, counts[i], sizes[i]);
        amount(query.total.uncertain, 0, 0);
        EXPECT_TRUE(query.incomplete);  // Recorded objects, never an exhaustive managed heap census.
        function_total(model, query, "Program.AllocateRetained(", 4097, 8503320);
        function_total(model, query, "Program.AllocateReleased(", 2, 48);
        if (i > 0) function_total(model, query, "Program.AllocateLarge(", i == 1 ? 2 : 1, i == 1 ? 131120 : 24);
        // The independent interval contains every retained/released allocation and excludes later housekeeping.
        auto cohort = model.query_outstanding_memory(at(ends[i]), {{at(12632974194), at(12633413016)}}, process,
                                                     AllocationKind::Managed);
        ASSERT_TRUE(cohort.error.empty()) << cohort.error;
        amount(cohort.total.known, 4099, 8503368);
        amount(cohort.total.uncertain, 0, 0);
    }
}
}  // namespace

TEST(ManagedAllocationFixture, DesktopCaptureMatchesIndependentGcFunctionAndCohortOracles) {
    const char* path = std::getenv("TRACE_MANAGED_DESKTOP_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_MANAGED_DESKTOP_FIXTURE to the pinned net48 capture";
    const auto bytes = fixture_bytes(path);
    ASSERT_EQ(bytes.size(), 3658727u);
    verify_clock(bytes, desktop_origin);
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(bytes, model, error)) << error;
    verify_history(model, 20133, 17881227);
    ASSERT_TRUE(model.profile().capabilities.managed_survival);
    ASSERT_FALSE(model.profile().allocations.empty());
    ASSERT_TRUE(model.profile().allocations.front().allocated_ts);
    EXPECT_DOUBLE_EQ(*model.profile().allocations.front().allocated_ts, at(12621974487));
    amount(origin_total(model, "Program.AllocateRetained("), 4106, 8519888);
    amount(origin_total(model, "Program.AllocateReleased("), 8206, 8651072);
    amount(origin_total(model, "Program+RetainedPayload..ctor(", 4120), 2048, 8437760);
    amount(origin_total(model, "Program+ReleasedPayload..ctor(", 2072), 4096, 8486912);
    amount(origin_total(model, "Program.AllocateLarge(", 131096), 1, 131096);
    for (const auto& observation : model.profile().managed_survival) EXPECT_FALSE(observation.address);
    desktop_queries(model);
    TraceModel reopened;
    round_trip(model, reopened);
    verify_history(reopened, 20133, 17881227);
    desktop_queries(reopened);
}

TEST(ManagedAllocationFixture, CoreCaptureKeepsProvenOriginsButRejectsContradictorySurvival) {
    const char* path = std::getenv("TRACE_MANAGED_CORE_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_MANAGED_CORE_FIXTURE to the pinned net8 capture";
    const auto bytes = fixture_bytes(path);
    ASSERT_EQ(bytes.size(), 3630912u);
    verify_clock(bytes, core_origin);
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(bytes, model, error)) << error;
    verify_history(model, 23652, 18483185);
    EXPECT_FALSE(model.profile().capabilities.managed_survival);
    EXPECT_TRUE(model.profile().managed_survival.empty());
    amount(origin_total(model, "Program.AllocateRetained("), 4106, 8519888);
    amount(origin_total(model, "Program.AllocateReleased("), 8206, 8651064);
    amount(origin_total(model, "Program+RetainedPayload..ctor(", 4120), 2048, 8437760);
    amount(origin_total(model, "Program+ReleasedPayload..ctor(", 2072), 4096, 8486912);
    bool runtime_note = false;
    for (const auto& warning : model.profile().quality.warnings)
        runtime_note |= warning.find("CoreCLR") != std::string::npos;
    EXPECT_TRUE(runtime_note);
    auto check_unknown = [](const TraceModel& imported) {
        auto query = imported.query_outstanding_memory(at(11016400054, core_origin), {}, process_for(imported, 7888),
                                                       AllocationKind::Managed);
        ASSERT_TRUE(query.error.empty()) << query.error;
        amount(query.total.known, 0, 0);
        amount(query.total.uncertain, 22849, 18419683);
        EXPECT_TRUE(query.incomplete);
    };
    check_unknown(model);
    TraceModel reopened;
    round_trip(model, reopened);
    verify_history(reopened, 23652, 18483185);
    EXPECT_FALSE(reopened.profile().capabilities.managed_survival);
    check_unknown(reopened);
}
