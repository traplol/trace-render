#include <gtest/gtest.h>
#include "parser/managed_methods.h"
#include <algorithm>

namespace {
void put(std::string& bytes, size_t at, uint64_t value, size_t width) {
    if (bytes.size() < at + width) bytes.resize(at + width);
    for (size_t i = 0; i < width; ++i) bytes[at + i] = char(value >> (8 * i));
}
void text(std::string& bytes, const std::string& value) {
    for (char c : value) {
        bytes += c;
        bytes += '\0';
    }
    bytes.append(2, '\0');
}
void method(ManagedMethods& methods, uint16_t id, uint64_t qpc, const std::string& name, uint64_t method_id = 1,
            bool rundown = false, uint16_t version = 2, uint64_t rejit = 0, uint32_t flags = 8,
            uint64_t address = 0x1000) {
    std::string payload(36, '\0');
    put(payload, 0, method_id, 8);
    put(payload, 8, 9, 8);
    put(payload, 16, address, 8);
    put(payload, 24, 0x100, 4);
    put(payload, 28, 0x06000001, 4);
    put(payload, 32, flags, 4);
    if (id >= 143) {
        text(payload, "Example.Worker");
        text(payload, name);
        text(payload, "instance void  (int32)");
    }
    if (version >= 1) put(payload, payload.size(), 1, 2);
    if (version >= 2) put(payload, payload.size(), rejit, 8);
    EtlRecord record;
    record.qpc = qpc;
    record.pid = 7;
    record.provider = rundown ? "a669021c-c450-4609-a035-5af59af4df18" : "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";
    record.event_id = id;
    record.version = version;
    record.pointer_size = 4;  // CLR method identity/range fields are UInt64 even on x86.
    record.payload = payload;
    ASSERT_TRUE(methods.consume(record));
}
void build(ManagedMethods& methods) {
    methods.build([](uint32_t pid, uint64_t) { return std::to_string(pid); });
}
}  // namespace

TEST(ManagedMethods, OrdersRundownLoadsAndUnloadsWithoutTreatingEnumerationAsBirth) {
    ManagedMethods methods;
    method(methods, 143, 150, "Replacement", 2);
    method(methods, 144, 120, "Original");
    method(methods, 143, 100, "Original", 1, true, 1);
    build(methods);
    auto* original = methods.find("7", 0x1010, 10);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->name, "Example.Worker.Original(int32)");
    EXPECT_FALSE(original->start_qpc);
    EXPECT_EQ(original->end_qpc, 120);
    EXPECT_EQ(methods.find("7", 0x1010, 120), nullptr);
    auto* replacement = methods.find("7", 0x1010, 150);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->name, "Example.Worker.Replacement(int32)");
    EXPECT_NE(replacement->symbol_id, original->symbol_id);
    EXPECT_EQ(methods.find("7", 0x1100, 150), nullptr);
    EXPECT_EQ(methods.find("8", 0x1010, 150), nullptr);
}

TEST(ManagedMethods, KeepsProcessRejitAndReloadIdentitiesDistinct) {
    ManagedMethods methods;
    method(methods, 143, 10, "First");
    method(methods, 144, 20, "First");
    method(methods, 143, 30, "First");
    method(methods, 143, 40, "Recompiled", 1, false, 2, 42);
    method(methods, 143, 210, "NewProcess", 1, true);
    methods.build([](uint32_t, uint64_t qpc) { return qpc < 200 ? "old" : "new"; });
    auto* first = methods.find("old", 0x1010, 15);
    auto* reloaded = methods.find("old", 0x1010, 35);
    auto* rejit = methods.find("old", 0x1010, 45);
    auto* reused_pid = methods.find("new", 0x1010, 205);
    ASSERT_TRUE(first && reloaded && rejit && reused_pid);
    EXPECT_NE(first->symbol_id, reloaded->symbol_id);
    EXPECT_NE(reloaded->symbol_id, rejit->symbol_id);
    EXPECT_NE(rejit->symbol_id, reused_pid->symbol_id);
    EXPECT_EQ(rejit->rejit_id, 42);
    EXPECT_EQ(rejit->start_qpc, 40);
    EXPECT_EQ(reloaded->end_qpc, 40);
    EXPECT_EQ(reused_pid->name, "Example.Worker.NewProcess(int32)");
}

TEST(ManagedMethods, NamesOnlyKnownRangesAndKeepsConflictsUnresolved) {
    ManagedMethods methods;
    method(methods, 141, 10, "", 1, false, 0);
    method(methods, 143, 20, "Named", 2, true, 0, 0, 8, 0x2000);
    method(methods, 143, 30, "Conflict", 2, true, 0, 0, 8, 0x2000);
    method(methods, 143, 40, "Named", 2, true, 0, 0, 8, 0x2000);
    build(methods);
    auto* nameless = methods.find("7", 0x1010, 15);
    auto* conflict = methods.find("7", 0x2010, 15);
    ASSERT_TRUE(nameless && conflict);
    EXPECT_TRUE(nameless->name.empty());
    EXPECT_TRUE(conflict->name.empty());
    EXPECT_EQ(methods.find("7", 0x3000, 15), nullptr);
    EXPECT_NE(std::find(methods.warnings().begin(), methods.warnings().end(),
                        "Conflicting CLR method names remain unresolved"),
              methods.warnings().end());
}

TEST(ManagedMethods, SharedCodeAndUnrecordedReuseDoNotPickAnArbitraryName) {
    ManagedMethods methods;
    method(methods, 143, 10, "SharedOne", 1, false, 2, 0, 12);
    method(methods, 143, 20, "SharedTwo", 2, false, 2, 0, 12);
    method(methods, 143, 30, "BeforeReuse", 3, false, 2, 0, 8, 0x2000);
    method(methods, 143, 100, "AfterReuse", 4, true, 2, 0, 8, 0x2080);
    build(methods);
    EXPECT_NE(methods.find("7", 0x1010, 15), nullptr);
    EXPECT_EQ(methods.find("7", 0x1010, 25), nullptr);
    EXPECT_NE(methods.find("7", 0x2080, 30), nullptr);
    EXPECT_EQ(methods.find("7", 0x2080, 50), nullptr);
    auto* later = methods.find("7", 0x2080, 100);
    ASSERT_NE(later, nullptr);
    EXPECT_EQ(later->name, "Example.Worker.AfterReuse(int32)");
}

TEST(ManagedMethods, UnsupportedOrMalformedTransitionsStopStaleAttribution) {
    for (bool extended : {false, true}) {
        ManagedMethods methods;
        method(methods, 143, 10, "BeforeGap");
        EtlRecord bad;
        bad.qpc = 20;
        bad.pid = 7;
        bad.provider = "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";
        bad.event_id = 143;
        bad.version = extended ? 2 : 99;
        bad.extended_data = extended;
        EXPECT_TRUE(methods.consume(bad));
        method(methods, 143, 30, "KnownReload", 2);
        method(methods, 143, 10, "OtherRange", 3, false, 2, 0, 8, 0x2000);
        method(methods, 144, 30, "OtherRange", 3, false, 2, 0, 8, 0x2000);
        method(methods, 143, 100, "RundownAfterGap", 4, true, 2, 0, 8, 0x2000);
        build(methods);
        EXPECT_NE(methods.find("7", 0x1010, 15), nullptr);
        EXPECT_EQ(methods.find("7", 0x1010, 25), nullptr);
        auto* after = methods.find("7", 0x1010, 35);
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(after->name, "Example.Worker.KnownReload(int32)");
        // An inferred lower bound from another method's unload is not a known
        // load and cannot restore attribution after skipped metadata.
        EXPECT_EQ(methods.find("7", 0x2010, 40), nullptr);
        EXPECT_FALSE(methods.warnings().empty());
    }
    ManagedMethods malformed;
    method(malformed, 143, 10, "BeforeBadPayload");
    EtlRecord bad;
    bad.qpc = 20;
    bad.pid = 7;
    bad.provider = "e13c0d23-ccbc-4e12-931b-d9cc2eee27e4";
    bad.event_id = 144;
    bad.version = 2;
    bad.payload = "short";
    EXPECT_TRUE(malformed.consume(bad));
    build(malformed);
    EXPECT_EQ(malformed.find("7", 0x1010, 25), nullptr);
}
