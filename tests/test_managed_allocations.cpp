#include <gtest/gtest.h>
#include "parser/managed_allocations.h"
#include <algorithm>
#include <stdexcept>

namespace {
constexpr const char* profiler = "8bc9e67b-ca34-4b9a-9442-8f75403f357b";
constexpr const char* clr = "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";
using Kind = ManagedAllocationEventKind;

void put(std::string& bytes, size_t at, uint64_t value, size_t width) {
    if (bytes.size() < at + width) bytes.resize(at + width);
    for (size_t i = 0; i < width; ++i) bytes[at + i] = char(value >> (8 * i));
}
EtlRecord record(const std::string& payload, uint16_t id = 1, uint16_t version = 4, const char* provider = profiler) {
    EtlRecord r;
    r.provider = provider;
    r.event_id = id;
    r.version = version;
    r.pointer_size = 8;
    r.pid = 42;
    r.qpc = 12345;
    r.payload = payload;
    return r;
}
std::string allocation_payload() {
    std::string bytes(45, '\0');
    put(bytes, 0, 0x100000004, 8);
    put(bytes, 8, 24, 4);
    put(bytes, 12, 25, 4);
    put(bytes, 16, 0x7fff12340000, 8);
    put(bytes, 24, 0x02000005, 4);
    put(bytes, 28, 1, 4);
    return bytes;
}
ManagedAllocationEvent event(Kind kind, uint64_t qpc, const std::string& process = "process") {
    ManagedAllocationEvent e;
    e.kind = kind;
    e.raw_qpc = qpc;
    e.ts_us = qpc / 10.0;
    e.pid = 42;
    e.process_id = process;
    e.clr_instance = 7;
    e.runtime_sku = 1;
    e.runtime_major = 4;
    e.gc_number = 1;
    e.gc_depth = 2;
    e.object_id = 4;
    e.size_bytes = 24;
    e.stack_frame_id = "origin";
    e.type_id = "payload";
    return e;
}
std::vector<ManagedAllocationEvent> complete_gc(const std::string& process = "process") {
    return {event(Kind::Runtime, 1, process), event(Kind::Allocate, 10, process), event(Kind::GcStart, 20, process),
            event(Kind::GcEnd, 30, process)};
}
ProfileData replay(std::vector<ManagedAllocationEvent> events, bool complete = true) {
    ProfileData profile;
    build_managed_allocations(events, profile, complete);
    return profile;
}
void expect_history_without_survival(const ProfileData& p, size_t allocations = 1) {
    EXPECT_TRUE(p.capabilities.managed_allocation_history);
    EXPECT_FALSE(p.capabilities.managed_survival);
    EXPECT_EQ(p.allocations.size(), allocations);
    EXPECT_TRUE(p.managed_survival.empty());
    EXPECT_FALSE(p.quality.warnings.empty());
    for (const auto& a : p.allocations) {
        EXPECT_FALSE(a.address);
        EXPECT_FALSE(a.freed_ts);
        EXPECT_EQ(a.end_state, AllocationEnd::Unknown);
    }
}
}  // namespace

TEST(ManagedAllocationDecode, ExactAllocationLayoutPreservesOpaqueIdentityAndOneInlineStack) {
    const auto bytes = allocation_payload();
    auto r = record(bytes);
    r.extended_data = true;
    r.extensions = {{6, {}, EtlExtendedStack{0, 8, {0x1000, 0x2000}}}, {99, "unknown", {}}};
    ManagedAllocationEvent decoded;
    ASSERT_TRUE(decode_managed_allocation(r, decoded));
    EXPECT_EQ(decoded.kind, Kind::Allocate);
    EXPECT_EQ(decoded.object_id, 0x100000004u);
    EXPECT_EQ(decoded.size_bytes, 24u);
    EXPECT_EQ(decoded.raw_qpc, 12345u);
    EXPECT_EQ(decoded.pid, 42u);
    EXPECT_EQ(decoded.type_id, "vs-type/00003412ff7f00000500000201000000000000000000000000");
    EXPECT_EQ(decoded.addresses, (std::vector<uint64_t>{0x1000, 0x2000}));
    EXPECT_TRUE(decoded.stack_frame_id.empty());
    EXPECT_TRUE(decoded.process_id.empty());
}

TEST(ManagedAllocationDecode, AmbiguousOrWrongWidthStackPartsDoNotInventAnOrigin) {
    const auto bytes = allocation_payload();
    auto r = record(bytes);
    for (const auto& stacks : std::vector<std::vector<EtlExtension>>{
             {},
             {{5, {}, EtlExtendedStack{0, 4, {0x1000}}}},
             {{6, {}, EtlExtendedStack{0, 8, {0x1000}}}, {6, {}, EtlExtendedStack{0, 8, {0x2000}}}}}) {
        r.extensions = stacks;
        ManagedAllocationEvent decoded;
        ASSERT_TRUE(decode_managed_allocation(r, decoded));
        EXPECT_EQ(decoded.kind, Kind::Allocate);
        EXPECT_EQ(decoded.size_bytes, 24u);
        EXPECT_TRUE(decoded.addresses.empty());
    }
}

TEST(ManagedAllocationDecode, RelevantUnknownVersionsAndTruncatedPayloadsBecomeGaps) {
    const auto valid = allocation_payload();
    for (size_t size = 0; size < valid.size(); ++size) {
        SCOPED_TRACE(size);
        const auto shortened = valid.substr(0, size);
        ManagedAllocationEvent decoded;
        ASSERT_TRUE(decode_managed_allocation(record(shortened), decoded));
        EXPECT_EQ(decoded.kind, Kind::Gap);
    }
    for (int defect = 0; defect < 6; ++defect) {
        SCOPED_TRACE(defect);
        auto bytes = valid;
        if (defect == 0) bytes += '\0';
        if (defect == 1) put(bytes, 12, UINT32_MAX, 4);
        if (defect == 2) put(bytes, 0, 0, 8);
        auto r = record(bytes);
        if (defect == 3) r.version = 3;
        if (defect == 4) r.pointer_size = 4;
        if (defect == 5) r.pid.reset();
        ManagedAllocationEvent decoded;
        ASSERT_TRUE(decode_managed_allocation(r, decoded));
        EXPECT_EQ(decoded.kind, Kind::Gap);
    }
    ManagedAllocationEvent decoded;
    auto unrelated = record(valid, 143, 1, clr);
    EXPECT_FALSE(decode_managed_allocation(unrelated, decoded));
    unrelated.provider = "another-provider";
    EXPECT_FALSE(decode_managed_allocation(unrelated, decoded));
    EXPECT_TRUE(decode_managed_allocation(record(valid, 99), decoded));
    EXPECT_EQ(decoded.kind, Kind::Gap);
}

TEST(ManagedAllocationDecode, CollectionCountsMustExhaustThePayload) {
    std::string bytes(24, '\0');
    put(bytes, 4, 2, 4);
    put(bytes, 8, 4, 8);
    put(bytes, 16, 0x100000008, 8);
    ManagedAllocationEvent decoded;
    ASSERT_TRUE(decode_managed_allocation(record(bytes, 2, 1), decoded));
    EXPECT_EQ(decoded.kind, Kind::Collected);
    EXPECT_EQ(decoded.collected_ids, (std::vector<uint64_t>{4, 0x100000008}));
    for (int defect = 0; defect < 5; ++defect) {
        SCOPED_TRACE(defect);
        auto broken = bytes;
        if (defect == 0) broken.pop_back();
        if (defect == 1) broken += '\0';
        if (defect == 2) put(broken, 4, UINT32_MAX, 4);
        if (defect == 3) put(broken, 0, 7, 4);
        ASSERT_TRUE(decode_managed_allocation(record(broken, 2, defect == 4 ? 2 : 1), decoded));
        EXPECT_EQ(decoded.kind, Kind::Gap);
    }
}

TEST(ManagedAllocationDecode, RuntimeAndGcUseExactVersionedClrFields) {
    std::string runtime(43, '\0');  // Fixed prefix, empty command line, GUID, then UTF-16 runtime path.
    put(runtime, 0, 7, 2);
    put(runtime, 2, 1, 2);
    put(runtime, 4, 8, 2);  // BCL major is distinct from the VM major used for survival support.
    put(runtime, 12, 4, 2);
    for (char c : std::string("clr.dll")) {
        runtime += c;
        runtime += '\0';
    }
    runtime.append(2, '\0');
    ManagedAllocationEvent decoded;
    ASSERT_TRUE(decode_managed_allocation(record(runtime, 187, 0, clr), decoded));
    EXPECT_EQ(decoded.kind, Kind::Runtime);
    EXPECT_EQ(decoded.clr_instance, 7);
    EXPECT_EQ(decoded.runtime_sku, 1);
    EXPECT_EQ(decoded.runtime_major, 4);
    ASSERT_TRUE(decode_managed_allocation(record(runtime, 187, 1, clr), decoded));
    EXPECT_EQ(decoded.kind, Kind::Gap);
    runtime.pop_back();
    ASSERT_TRUE(decode_managed_allocation(record(runtime, 187, 0, clr), decoded));
    EXPECT_EQ(decoded.kind, Kind::Gap);

    for (bool start : {true, false}) {
        std::string bytes(start ? 26 : 10, '\0');
        put(bytes, 0, 91, 4);
        put(bytes, 4, 2, 4);
        put(bytes, start ? 16 : 8, 7, 2);
        if (start) put(bytes, 12, 1, 4);
        const auto r = record(bytes, start ? 1 : 2, start ? 2 : 1, clr);
        ASSERT_TRUE(decode_managed_allocation(r, decoded));
        EXPECT_EQ(decoded.kind, start ? Kind::GcStart : Kind::GcEnd);
        EXPECT_EQ(decoded.gc_number, 91u);
        EXPECT_EQ(decoded.gc_depth, 2u);
        EXPECT_EQ(decoded.clr_instance, 7);
        if (start) EXPECT_EQ(decoded.gc_type, 1u);
        ASSERT_TRUE(decode_managed_allocation(record(bytes, r.event_id, start ? 1 : 2, clr), decoded));
        EXPECT_EQ(decoded.kind, Kind::Gap);
        bytes += '\0';
        ASSERT_TRUE(decode_managed_allocation(record(bytes, r.event_id, r.version, clr), decoded));
        EXPECT_EQ(decoded.kind, Kind::Gap);
    }
}

TEST(ManagedAllocationReplay, FullGcObservationsKeepOriginsButNeverInventExactDeathOrHeapAddresses) {
    auto events = complete_gc();
    auto second = event(Kind::Allocate, 11);
    second.object_id = 8;
    second.size_bytes = 40;
    events.push_back(second);
    auto start = event(Kind::GcStart, 40), end = event(Kind::GcEnd, 50);
    start.gc_number = end.gc_number = 2;
    auto dead = event(Kind::Collected, 45);
    dead.collected_ids = {4};
    events.insert(events.end(), {end, dead, start});  // Physical ETL order need not be timestamp order.
    const auto p = replay(events);
    ASSERT_EQ(p.allocations.size(), 2u);
    EXPECT_TRUE(p.capabilities.managed_survival);
    EXPECT_TRUE(p.quality.incomplete_capture);
    EXPECT_FALSE(p.quality.sampled_allocations);
    ASSERT_EQ(p.managed_survival.size(), 4u);
    size_t positive = 0, negative = 0;
    for (const auto& observation : p.managed_survival) {
        EXPECT_FALSE(observation.address);
        if (observation.survived) {
            ++positive;
            EXPECT_TRUE(observation.ts == 3 || observation.ts == 5);
        } else {
            ++negative;
            EXPECT_EQ(observation.allocation_id, p.allocations[0].id);
            EXPECT_DOUBLE_EQ(observation.ts, 5);  // Complete GC end, not the batch emission at 4.5 us.
        }
    }
    EXPECT_EQ(positive, 3u);
    EXPECT_EQ(negative, 1u);
    for (const auto& a : p.allocations) {
        EXPECT_EQ(a.stack_frame_id, "origin");
        EXPECT_EQ(a.kind, AllocationKind::Managed);
        EXPECT_FALSE(a.address);
        EXPECT_FALSE(a.freed_ts);
        EXPECT_EQ(a.end_state, AllocationEnd::Unknown);
    }
}

TEST(ManagedAllocationReplay, MinorGcAbsenceIsUsefulButDoesNotProveOtherObjectsSurvived) {
    auto events = complete_gc();
    events[2].gc_depth = events[3].gc_depth = 0;
    auto second = event(Kind::Allocate, 11);
    second.object_id = 8;
    auto dead = event(Kind::Collected, 25);
    dead.collected_ids = {4};
    events.insert(events.end(), {second, dead});
    const auto p = replay(events);
    ASSERT_EQ(p.managed_survival.size(), 1u);
    EXPECT_FALSE(p.managed_survival[0].survived);
    EXPECT_EQ(p.managed_survival[0].allocation_id, p.allocations[0].id);
    EXPECT_EQ(p.managed_survival[0].ts, 3);
    expect_history_without_survival(replay({events[0], events[1]}));
}

TEST(ManagedAllocationReplay, SurvivalRequiresOneDesktopClrFourInstancePerProcess) {
    for (int mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode);
        auto events = complete_gc();
        if (mode == 0) events.erase(events.begin());
        if (mode == 1) events[0].runtime_sku = 2;
        if (mode == 2) events[0].runtime_major = 5;
        if (mode == 3) {
            auto other = events[0];
            other.clr_instance = 8;
            events.push_back(other);
        }
        if (mode == 4) events[2].clr_instance = events[3].clr_instance = 8;
        if (mode == 5) {
            auto other = events[0];
            other.runtime_sku = 2;
            events.push_back(other);
        }
        expect_history_without_survival(replay(events));
    }
    auto repeated_runtime = complete_gc();
    repeated_runtime.push_back(repeated_runtime.front());
    EXPECT_TRUE(replay(repeated_runtime).capabilities.managed_survival);
}

TEST(ManagedAllocationReplay, GapsLostRecordsReusedIdsAndBrokenCollectionsInvalidateAllObservations) {
    for (int defect = 0; defect < 11; ++defect) {
        SCOPED_TRACE(defect);
        auto events = complete_gc();
        if (defect == 0) events.push_back(event(Kind::Gap, 40));
        if (defect == 1) events.push_back(event(Kind::Allocate, 40));
        if (defect == 2) events[3].gc_number = 2;
        if (defect == 3) events.pop_back();
        if (defect == 4) events.erase(events.begin() + 2);
        if (defect == 5) events[2].gc_type = 1;
        if (defect == 6) events[3].gc_depth = 1;
        if (defect == 7) events.push_back(event(Kind::GcStart, 25));
        if (defect == 8) {
            auto during = event(Kind::Allocate, 25);
            during.object_id = 8;
            events.push_back(during);
        }
        if (defect >= 9) {
            auto dead = event(Kind::Collected, 25);
            dead.collected_ids = defect == 9 ? std::vector<uint64_t>{999} : std::vector<uint64_t>{4, 4};
            events.push_back(dead);
        }
        expect_history_without_survival(replay(events), defect == 1 || defect == 8 ? 2 : 1);
    }
    expect_history_without_survival(replay(complete_gc(), false));
}

TEST(ManagedAllocationReplay, IdenticalProfilerIdsAndFailuresStayScopedToProcessLifetimes) {
    auto events = complete_gc("old-process");
    auto other = complete_gc("new-process");  // Same OS PID and object ID, different normalized process lifetime.
    events.insert(events.end(), other.begin(), other.end());
    events.push_back(event(Kind::Gap, 40, "old-process"));
    auto p = replay(events);
    ASSERT_EQ(p.allocations.size(), 2u);
    EXPECT_NE(p.allocations[0].id, p.allocations[1].id);
    ASSERT_EQ(p.managed_survival.size(), 1u);
    EXPECT_EQ(p.managed_survival[0].allocation_id, p.allocations[1].id);
    EXPECT_TRUE(p.capabilities.managed_survival);
    auto unknown = event(Kind::Gap, 40, "");
    unknown.pid = 0;
    events.push_back(unknown);
    expect_history_without_survival(replay(events), 2);
}

TEST(ManagedAllocationReplay, CancellationDoesNotContinueThroughTheHistory) {
    auto events = complete_gc();
    ProfileData p;
    EXPECT_THROW(build_managed_allocations(events, p, true, [](const char*, float) { return false; }),
                 std::runtime_error);
    EXPECT_TRUE(p.allocations.empty());
}

TEST(ManagedAllocationReplay, ReuseAfterCollectionStillCannotJoinProfilerIdentities) {
    auto events = complete_gc();
    auto dead = event(Kind::Collected, 25);
    dead.collected_ids = {4};
    auto reused = event(Kind::Allocate, 40);
    reused.stack_frame_id = "different-origin";
    reused.size_bytes = 80;
    events.insert(events.end(), {dead, reused});
    const auto p = replay(events);
    expect_history_without_survival(p, 2);
    ASSERT_EQ(p.allocations.size(), 2u);
    EXPECT_NE(p.allocations[0].id, p.allocations[1].id);
    EXPECT_EQ(p.allocations[0].stack_frame_id, "origin");
    EXPECT_EQ(p.allocations[1].stack_frame_id, "different-origin");
    EXPECT_EQ(p.allocations[0].size_bytes, 24u);
    EXPECT_EQ(p.allocations[1].size_bytes, 80u);
}

TEST(ManagedAllocationReplay, BrokenGcSequenceInvalidatesEarlierPositiveObservations) {
    for (uint32_t next : {0u, 1u, 3u}) {
        SCOPED_TRACE(next);
        auto events = complete_gc();
        auto start = event(Kind::GcStart, 40), end = event(Kind::GcEnd, 50);
        start.gc_number = end.gc_number = next;
        events.insert(events.end(), {start, end});
        expect_history_without_survival(replay(events));
    }
}
