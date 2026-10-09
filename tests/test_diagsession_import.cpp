#include <gtest/gtest.h>
#include "parser/diagsession_import.h"
#include "parser/diagsession_container.h"
#include "parser/profile_io.h"
#include "parser/trace_parser.h"
#include "platform/file_loader.h"
#include "symbols/native_symbol_resolver.h"
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <filesystem>

namespace {
using json = nlohmann::json;
void put(std::string& bytes, size_t at, uint64_t value, size_t width) {
    if (bytes.size() < at + width) bytes.resize(at + width);
    for (size_t i = 0; i < width; ++i) bytes[at + i] = char(value >> (i * 8));
}
std::string utf16(const std::string& value) {
    std::string result;
    for (char c : value) {
        result += c;
        result += '\0';
    }
    result.append(2, '\0');
    return result;
}
std::string event(uint8_t group, uint8_t opcode, uint16_t version, uint64_t qpc, const std::string& payload,
                  bool perf = false) {
    size_t header = perf ? 16 : 32;
    std::string result(header, '\0');
    put(result, 0, version, 2);
    put(result, 2, perf ? 17 : 2, 1);
    put(result, 3, 0xc0, 1);
    put(result, 4, header + payload.size(), 2);
    put(result, 6, opcode, 1);
    put(result, 7, group, 1);
    put(result, perf ? 8 : 16, qpc, 8);
    if (!perf) {
        put(result, 8, 9, 4);
        put(result, 12, 7, 4);
    }
    result += payload;
    result.resize((result.size() + 7) & ~size_t(7));
    return result;
}
std::string sample(uint64_t qpc, uint64_t ip, uint16_t count = 1) {
    std::string p(16, '\0');
    put(p, 0, ip, 8);
    put(p, 8, 9, 4);
    put(p, 12, count, 2);
    put(p, 14, 0x1234, 2);
    return event(15, 46, 2, qpc, p, true);
}
std::string process(uint8_t opcode, uint64_t qpc, uint64_t key) {
    std::string p(40, '\0');
    put(p, 0, key, 8);
    put(p, 8, 7, 4);
    p += "workload.exe";
    p += '\0';
    return event(3, opcode, 4, qpc, p);
}
std::string thread(uint8_t opcode, uint64_t qpc) {
    std::string p(72, '\0');
    put(p, 0, 7, 4);
    put(p, 4, 9, 4);
    p += utf16("worker");
    return event(5, opcode, 3, qpc, p);
}
std::string module(uint8_t opcode, uint64_t qpc, const std::string& name) {
    std::string p(56, '\0');
    put(p, 0, 0x1000, 8);
    put(p, 8, 0x1000, 8);
    put(p, 16, 7, 4);
    p += utf16(name);
    return event(opcode == 10 ? 3 : 20, opcode, 3, qpc, p);
}
std::string stack(uint64_t qpc, std::initializer_list<uint64_t> frames) {
    std::string p(16, '\0');
    put(p, 0, qpc, 8);
    put(p, 8, 7, 4);
    put(p, 12, 9, 4);
    for (auto frame : frames) put(p, p.size(), frame, 8);
    return event(24, 32, 2, qpc + 1, p, true);
}
std::string heap(uint8_t opcode, uint64_t qpc, uint64_t address, uint64_t size = 0, uint16_t version = 2) {
    std::string p(opcode == 33 ? 28 : opcode == 36 ? 20 : 12, '\0');
    put(p, 0, 0x9000, 8);
    if (opcode == 33) {
        put(p, 8, size, 8);
        put(p, 16, address, 8);
    }
    if (opcode == 36) put(p, 8, address, 8);
    return event(16, opcode, version, qpc, p);
}
std::string manifest_heap() {
    std::string bytes(80, '\0');
    put(bytes, 0, 80, 2);
    put(bytes, 2, 19, 1);
    put(bytes, 3, 0xc0, 1);
    put(bytes, 8, 9, 4);
    put(bytes, 12, 7, 4);
    put(bytes, 16, 1300, 8);
    put(bytes, 24, 0x222962ab, 4);
    put(bytes, 28, 0x6180, 2);
    put(bytes, 30, 0x4b88, 2);
    put(bytes, 32, 0x4aa2f2756b3425a8ULL, 8);
    put(bytes, 40, 33, 2);
    put(bytes, 42, 2, 1);  // Manifest ID 33, opcode 0 is not the classic schema.
    return bytes;
}
std::string extension(uint16_t type, const std::string& data, bool more = false) {
    std::string bytes(8, '\0');
    put(bytes, 0, (8 + data.size() + 7) & ~size_t(7), 2);
    put(bytes, 2, type, 2);
    put(bytes, 4, more, 2);
    put(bytes, 6, data.size(), 2);
    bytes += data;
    bytes.resize((bytes.size() + 7) & ~size_t(7), char(0xa5));
    return bytes;
}
std::string extended_event(const std::string& items, const std::string& payload, uint8_t type = 19) {
    auto bytes = manifest_heap();
    put(bytes, 2, type, 1);
    put(bytes, 4, 1, 2);
    bytes += items + payload;
    put(bytes, 0, bytes.size(), 2);
    bytes.resize((bytes.size() + 7) & ~size_t(7));
    return bytes;
}
std::string clr_method(uint16_t id, uint64_t qpc, uint64_t method_id, const std::string& name) {
    std::string payload(36, '\0');
    put(payload, 0, method_id, 8);
    put(payload, 8, 42, 8);
    put(payload, 16, 0x2000, 8);
    put(payload, 24, 0x100, 4);
    put(payload, 28, 0x06000001, 4);
    put(payload, 32, 8, 4);
    payload += utf16("Example") + utf16(name) + utf16("void  ()");
    put(payload, payload.size(), 1, 2);
    put(payload, payload.size(), 0, 8);
    std::string bytes(80, '\0');
    put(bytes, 0, bytes.size() + payload.size(), 2);
    put(bytes, 2, 19, 1);
    put(bytes, 3, 0xc0, 1);
    put(bytes, 8, 9, 4);
    put(bytes, 12, 7, 4);
    put(bytes, 16, qpc, 8);
    put(bytes, 24, 0xe13c0d23, 4);
    put(bytes, 28, 0xccbc, 2);
    put(bytes, 30, 0x4e12, 2);
    put(bytes, 32, 0xe427ee2eccd91b93ULL, 8);
    put(bytes, 40, id, 2);
    put(bytes, 42, 2, 1);
    put(bytes, 45, id == 143 ? 37 : 38, 1);
    bytes += payload;
    bytes.resize((bytes.size() + 7) & ~size_t(7));
    return bytes;
}
std::string etl(const std::string& records, uint32_t loss = 0) {
    std::string header(284, '\0');
    put(header, 0, 4096, 4);
    put(header, 16, 130000000000010000ULL, 8);
    put(header, 36, 1, 4);
    put(header, 44, 8, 4);
    put(header, 48, loss, 4);
    put(header, 256, 10000000, 8);
    put(header, 264, 130000000000000000ULL, 8);
    put(header, 272, 1, 4);
    auto body = event(0, 0, 2, 1000, header) + records;
    std::string bytes(72, '\0');
    put(bytes, 0, 72 + body.size(), 4);
    put(bytes, 4, 72 + body.size(), 4);
    put(bytes, 54, 4, 2);
    bytes += body;
    return bytes;
}
std::string session(const std::string& trace, const std::string& second = "", const std::string& label = "cpu.etl",
                    const std::string& embedded_pdb = "") {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) throw std::runtime_error("ZIP test init");
    std::string metadata = R"(<Package xmlns="urn:diagnosticshub-package-metadata-2-1"><Content>
      <Resource Type="DiagnosticsHub.Resource.EtlFile" Id="{fixture}" Name="cpu.etl"
      ResourcePackageUriPrefix="fixture" IsDirectoryOnDisk="false"/>)";
    if (!second.empty())
        metadata += R"(<Resource Type="DiagnosticsHub.Resource.EtlFile" Id="{second}"
      Name="second.etl" ResourcePackageUriPrefix="second" IsDirectoryOnDisk="false"/>)";
    if (!embedded_pdb.empty())
        metadata += R"(<Resource Type="DiagnosticsHub.Resource.EmbeddedPdbs" Id="{symbols}"
      Name="symbols" ResourcePackageUriPrefix="symbols" IsDirectoryOnDisk="true"/>)";
    metadata += "</Content></Package>";
    metadata.replace(metadata.find("cpu.etl"), 7, label);
    mz_zip_writer_add_mem(&zip, "metadata.xml", metadata.data(), metadata.size(), MZ_BEST_SPEED);
    // Unreferenced ETL-like entries must never be parsed.
    mz_zip_writer_add_mem(&zip, "decoy.etl", "not an ETL", 10, MZ_BEST_SPEED);
    mz_zip_writer_add_mem(&zip, ("fixture/" + label).c_str(), trace.data(), trace.size(), MZ_BEST_SPEED);
    if (!second.empty()) mz_zip_writer_add_mem(&zip, "second/second.etl", second.data(), second.size(), MZ_BEST_SPEED);
    if (!embedded_pdb.empty())
        mz_zip_writer_add_mem(&zip, "symbols/renamed.bin", embedded_pdb.data(), embedded_pdb.size(), MZ_BEST_SPEED);
    void* data = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &data, &size)) throw std::runtime_error("ZIP test finalize");
    std::string result(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_writer_end(&zip);
    return result;
}
}  // namespace

TEST(EtlReader, RejectsTruncationBadSizesClocksAndUnsupportedFraming) {
    auto valid = etl(sample(1200, 0x1010));
    std::vector<std::pair<size_t, uint64_t>> corruptions = {
        {0, 71},           {4, 0xffffffff}, {54, 0}, {72 + 4, 1}, {72 + 2, 9}, {72 + 32 + 44, 16}, {72 + 32 + 272, 2},
        {72 + 32 + 36, 2},
    };
    for (auto [offset, value] : corruptions) {
        auto changed = valid;
        put(changed, offset, value, offset == 54 || offset == 76 ? 2 : offset == 74 ? 1 : 4);
        EtlFileInfo info;
        std::string error;
        EXPECT_FALSE(read_etl(changed, info, {}, error)) << offset;
        EXPECT_FALSE(error.empty());
    }
    for (size_t size : {size_t(0), size_t(71), valid.size() - 1}) {
        EtlFileInfo info;
        std::string error;
        EXPECT_FALSE(read_etl(std::string_view(valid).substr(0, size), info, {}, error));
    }
    EtlFileInfo info;
    std::string error;
    EXPECT_FALSE(read_etl(valid, info, {}, error, [](auto, float) { return false; }));
    EXPECT_EQ(error, "Import canceled");
}

TEST(EtlReader, ReadsXpressBuffersAndRejectsMalformedMatchesOrUnknownCompression) {
    auto body = sample(1100, 0x1010);
    std::string compressed;
    for (size_t at = 0; at < body.size(); at += 32) {
        compressed.append(4, '\0');
        compressed += body.substr(at, 32);
    }
    auto header = etl("");
    put(header, 44, 3, 4);  // XPRESS format in header buffer state.
    put(header, 72 + 32 + 32, 0x04000000, 4);
    put(header, 72 + 32 + 36, 2, 4);
    std::string buffer(72, '\0');
    put(buffer, 0, 72 + compressed.size(), 4);
    put(buffer, 4, 72 + body.size(), 4);
    put(buffer, 52, 0x40, 2);
    buffer += compressed;
    auto data = header + buffer;
    EtlFileInfo info;
    std::string error;
    size_t samples = 0;
    ASSERT_TRUE(read_etl(
        data, info,
        [&](const auto&, const auto& r) {
            samples += r.group == 15 && r.opcode == 46;
            return true;
        },
        error))
        << error;
    EXPECT_EQ(samples, 1u);
    auto malformed = data;
    put(malformed, header.size() + 72, 0x80000000, 4);
    put(malformed, header.size() + 76, 0, 2);  // Match before any output exists.
    EXPECT_FALSE(read_etl(malformed, info, {}, error));
    EXPECT_NE(error.find("XPRESS match"), std::string::npos);
    put(data, 44, 2, 4);  // LZNT1 has no supported decoder.
    EXPECT_FALSE(read_etl(data, info, {}, error));
    EXPECT_NE(error.find("Unsupported ETL buffer compression 2"), std::string::npos);
}

TEST(EtlReader, SplitsLinkedExtensionsAndPreservesStackWidthsDuplicatesAndUnknownItems) {
    std::string stack32, stack64;
    put(stack32, 0, 0xfedcba9876543210ULL, 8);
    put(stack32, 8, 0xffffffff, 4);
    put(stack32, 12, 0, 4);
    put(stack32, 16, 0x81234567, 4);
    put(stack64, 0, 0xfedcba9876543210ULL, 8);
    put(stack64, 8, 0xffff800000001000ULL, 8);
    put(stack64, 16, 0xffff800000001000ULL, 8);  // Recursion remains recorded.
    const std::string unknown("\0\1\2\3\4", 5);
    auto items = extension(5, stack32, true) + extension(0xffff, unknown, true) + extension(6, stack64, true) +
                 extension(6, stack64);
    // UserData may itself look like another extension. Only Linkage continues a chain.
    auto payload = extension(6, stack64) + "user data";
    for (uint8_t header : {18, 19}) {
        auto data = etl(extended_event(items, payload, header) + sample(1400, 0x1234));
        EtlFileInfo info;
        std::string error;
        size_t extended = 0, following_samples = 0;
        auto check = [&](const EtlRecord& r) {
            ASSERT_EQ(r.extensions.size(), 4u);
            EXPECT_EQ(r.payload, payload);
            EXPECT_EQ(r.pointer_size, header == 18 ? 4 : 8);
            const auto& a = r.extensions[0];
            EXPECT_EQ(a.type, 5);
            EXPECT_EQ(a.data, stack32);
            ASSERT_TRUE(a.stack);
            EXPECT_EQ(a.stack->pointer_size, 4);
            EXPECT_EQ(a.stack->match_id, 0xfedcba9876543210ULL);
            EXPECT_EQ(a.stack->addresses, (std::vector<uint64_t>{0xffffffff, 0, 0x81234567}));
            EXPECT_EQ(r.extensions[1].type, 0xffff);
            EXPECT_EQ(r.extensions[1].data, unknown);
            EXPECT_FALSE(r.extensions[1].stack);
            for (size_t i : {2u, 3u}) {
                const auto& b = r.extensions[i];
                EXPECT_EQ(b.type, 6);
                EXPECT_EQ(b.data, stack64);
                ASSERT_TRUE(b.stack);
                EXPECT_EQ(b.stack->pointer_size, 8);
                EXPECT_EQ(b.stack->match_id, a.stack->match_id);
                EXPECT_EQ(b.stack->addresses, (std::vector<uint64_t>{0xffff800000001000ULL, 0xffff800000001000ULL}));
            }
        };
        ASSERT_TRUE(read_etl(
            data, info,
            [&](const auto&, const EtlRecord& r) {
                if (r.extended_data) {
                    ++extended;
                    check(r);
                } else {
                    EXPECT_TRUE(r.extensions.empty());
                    if (r.group == 15 && r.opcode == 46) ++following_samples;
                }
                return true;
            },
            error))
            << error;
        EXPECT_EQ(info.records_read, 3u);
        EXPECT_EQ(extended, 1u);
        EXPECT_EQ(following_samples, 1u);
    }
}

TEST(EtlReader, RejectsMalformedExtendedItemsWithinRecordBoundaries) {
    auto valid = extension(6, std::string(16, '\0'));
    std::vector<std::string> malformed;
    for (size_t size = 0; size < 8; ++size) malformed.push_back(valid.substr(0, size));
    for (auto [offset, value] : std::vector<std::pair<size_t, uint16_t>>{
             {0, 0}, {0, 7}, {0, 23}, {0, 0xffff}, {6, 0xffff}, {6, 15}, {4, 2}, {4, 1}}) {
        auto changed = valid;
        put(changed, offset, value, 2);
        malformed.push_back(changed);
    }
    malformed.push_back(valid.substr(0, valid.size() - 1));
    malformed.push_back(extension(5, std::string(12, '\0')).substr(0, 20));  // Missing item alignment.
    for (auto [type, size] : std::vector<std::pair<uint16_t, size_t>>{{5, 0}, {5, 7}, {5, 10}, {6, 12}})
        malformed.push_back(extension(type, std::string(size, '\0')));
    for (size_t i = 0; i < malformed.size(); ++i) {
        SCOPED_TRACE(i);
        // A following event must not supply missing extension bytes or padding.
        auto bytes = etl(extended_event(malformed[i], "") + sample(1400, 0x1234));
        EtlFileInfo info;
        std::string error;
        size_t callbacks = 0;
        EXPECT_FALSE(read_etl(
            bytes, info,
            [&](const auto&, const auto&) {
                ++callbacks;
                return true;
            },
            error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(callbacks, 1u);  // Only the logfile header is complete.
    }
}

TEST(EtlReader, ControlledManagedCaptureSeparatesStacksFromAllocationUserData) {
    const char* path = std::getenv("TRACE_MANAGED_ALLOCATION_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_MANAGED_ALLOCATION_FIXTURE to managed-allocation-survival.diagsession";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    DiagsessionContainer container;
    std::string error;
    ASSERT_TRUE(container.open(bytes, error)) << error;
    // Constants come from a separate raw-byte census and an external ETL decoder.
    const std::string first_payload(
        "\x04\x00\x00\x00\x00\x00\x00\x00\xf8\x1f\x00\x00\x1d\x00\x00\x00"
        "\x00\x40\xbc\xc0\xfc\x7f\x00\x00\x98\x00\x00\x02\x01\x00\x00\x00"
        "\x00\x00\x00\x00\x01\x00\x00\x00\xfc\x03\x00\x00\x00\x04\x00\x00\x00",
        49);
    const std::vector<uint64_t> first_stack = {
        0x7ffd580c05d4ULL, 0x7ffd580690e0ULL, 0x7ffd2fc84b7fULL, 0x7ffd2fc849c3ULL, 0x7ffd2fc89bb7ULL,
        0x7ffd209ab1a1ULL, 0x7ffd2094f9afULL, 0x7ffd209ebf6dULL, 0x7ffd208f52f3ULL, 0x7ffd2079afd9ULL,
        0x7ffd2079674fULL, 0x7ffd207965a8ULL, 0x7ffd2089275cULL, 0x7ffd2081b842ULL, 0x7ffd20846932ULL,
        0x7ffd2084b9a6ULL, 0x7ffd2089c8f9ULL, 0x7ffd2089c895ULL, 0x7ffd2089c7cbULL, 0x7ffd2088ad69ULL,
        0x7ffd274f3931ULL, 0x7ffd275104d5ULL, 0x7ffd275127cfULL, 0x7ffd309dd56bULL, 0x7ffd309e029cULL,
        0x7ffd309e2676ULL, 0x7ffd309e079dULL, 0x7ffd309d8998ULL, 0x7ff76c41feacULL, 0x7ff76c420316ULL,
        0x7ff76c421a58ULL, 0x7ffd56ae4cc0ULL, 0x7ffd5809edbbULL};
    size_t resources = 0, records = 0, allocations = 0, extended = 0, invalid = 0, addresses = 0, user_bytes = 0;
    for (size_t i = 0; i < container.resources().size(); ++i) {
        const auto& resource = container.resources()[i];
        if (resource.directory || resource.type != "DiagnosticsHub.Resource.EtlFile") continue;
        ++resources;
        std::vector<uint8_t> data;
        ASSERT_TRUE(container.read_resource(i, data, error)) << error;
        EtlFileInfo info;
        ASSERT_TRUE(read_etl(
            {reinterpret_cast<const char*>(data.data()), data.size()}, info,
            [&](const auto&, const EtlRecord& r) {
                extended += r.extended_data;
                if (r.provider != "8bc9e67b-ca34-4b9a-9442-8f75403f357b" || r.event_id != 1 || r.version != 4)
                    return true;
                ++allocations;
                user_bytes += r.payload.size();
                if (!r.extended_data || r.extensions.size() != 1 || r.extensions[0].type != 6 ||
                    !r.extensions[0].stack || r.extensions[0].stack->pointer_size != 8 ||
                    r.extensions[0].stack->match_id != 0) {
                    ++invalid;
                    return true;
                }
                addresses += r.extensions[0].stack->addresses.size();
                if (allocations == 1) {
                    EXPECT_EQ(r.qpc, 11841591261ULL);
                    EXPECT_EQ(r.pid, 4688u);
                    EXPECT_EQ(r.tid, 2900u);
                    EXPECT_EQ(r.extensions[0].data.size(), 272u);
                    EXPECT_EQ(r.extensions[0].stack->addresses, first_stack);
                    EXPECT_EQ(r.payload, first_payload);
                }
                return true;
            },
            error))
            << error;
        records += info.records_read;
        EXPECT_EQ(info.lost_events, 0u);
        EXPECT_EQ(info.lost_buffers, 0u);
        EXPECT_FALSE(info.buffer_loss_flag);
    }
    EXPECT_EQ(resources, 1u);
    EXPECT_EQ(records, 104170u);
    EXPECT_EQ(allocations, 23188u);
    EXPECT_EQ(extended, allocations);
    EXPECT_EQ(invalid, 0u);
    EXPECT_EQ(addresses, 956155u);
    EXPECT_EQ(user_bytes, 1130280u);
}

TEST(DiagsessionImport, PreservesObservationsAndReusedProcessThreadModuleIdentities) {
    std::string interval(12, '\0');
    put(interval, 4, 10000, 4);
    auto records = process(1, 1010, 42) + thread(1, 1020) + module(10, 1030, "old.dll") +
                   event(15, 73, 3, 1040, interval, true) + sample(1100, 0x1099, 17) + stack(1100, {0x1010, 0x1020}) +
                   stack(1100, {0xffff800000001000ULL}) + module(2, 1150, "old.dll") + thread(2, 1160) +
                   process(2, 1170, 42) + process(1, 1180, 99) + thread(1, 1190) + module(10, 1195, "new.dll") +
                   sample(1200, 0x1010) + stack(1200, {0x1010, 0xdeadbeef});
    auto data = session(etl(records));
    TraceModel model;
    TraceParser parser;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), model)) << parser.error_message();
    ASSERT_EQ(model.events().size(), 2u);
    const auto& first = model.events()[0];
    const auto& second = model.events()[1];
    EXPECT_DOUBLE_EQ(first.ts, 10);
    EXPECT_DOUBLE_EQ(second.ts, 20);
    EXPECT_EQ(first.kind, EventKind::Sample);
    EXPECT_DOUBLE_EQ(first.dur, 0);
    EXPECT_DOUBLE_EQ(first.sample_weight, 1);
    EXPECT_DOUBLE_EQ(first.sample_cpu_time, 1000);
    EXPECT_NE(first.process_instance_id, second.process_instance_id);
    auto a = json::parse(model.args()[first.args_idx]);
    auto b = json::parse(model.args()[second.args_idx]);
    EXPECT_EQ(a["raw_qpc"], 1100);
    EXPECT_EQ(a["raw_sample_ip"], 0x1099);
    EXPECT_EQ(a["sample_count"], 17);
    EXPECT_EQ(a["sample_flags"], 0x1234);
    EXPECT_EQ(a["raw_stack_parts"].size(), 2u);
    EXPECT_NE(a["thread_instance_id"], b["thread_instance_id"]);
    EXPECT_EQ(a["thread_end_qpc"], 1160);
    auto first_stack = model.build_sample_stack(0);
    auto second_stack = model.build_sample_stack(1);
    ASSERT_EQ(first_stack.size(), 3u);
    ASSERT_EQ(second_stack.size(), 2u);
    const auto& old_frame = model.stack_frames()[first_stack[1]];
    const auto& new_frame = model.stack_frames()[second_stack[1]];
    EXPECT_NE(old_frame.module_id, new_frame.module_id);
    EXPECT_EQ(model.stack_frames()[second_stack[0]].address, 0xdeadbeef);
    EXPECT_EQ(model.stack_frames()[second_stack[0]].module_id, UINT32_MAX);
    ASSERT_EQ(model.profile().processes.size(), 2u);
    EXPECT_EQ(model.profile().processes[0].end_ts, 17);
    EXPECT_FALSE(model.profile().processes[1].end_ts);
    std::string saved, error;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    TraceModel restored;
    ASSERT_TRUE(read_profile(saved, restored, error)) << error;
    EXPECT_EQ(restored.args(), model.args());
    EXPECT_EQ(restored.events().size(), 2u);
}

TEST(DiagsessionImport, RundownIsNotLoadOrUnloadAndUnknownAttributionStaysUnknown) {
    auto data = session(etl(process(3, 1010, 42) + thread(3, 1020) + module(3, 1030, "old.dll") + sample(1100, 0x1010) +
                                module(4, 1200, "old.dll") + process(4, 1200, 42),
                            3));
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    ASSERT_EQ(model.profile().modules.size(), 1u);
    EXPECT_FALSE(model.profile().modules[0].load_ts);
    EXPECT_FALSE(model.profile().modules[0].unload_ts);
    EXPECT_FALSE(model.profile().processes[0].start_ts);
    EXPECT_FALSE(model.profile().processes[0].end_ts);
    EXPECT_TRUE(model.profile().quality.incomplete_capture);
    EXPECT_EQ(model.profile().quality.lost_events, 3);
    EXPECT_LT(model.events()[0].sample_cpu_time, 0);
    data = session(etl(sample(1100, 0xabcdef)));
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    auto args = json::parse(model.args()[model.events()[0].args_idx]);
    EXPECT_FALSE(args["pid_known"].get<bool>());
    EXPECT_FALSE(args.contains("thread_instance_id"));
    EXPECT_NE(model.get_string(model.events()[0].process_instance_id).find("unknown"), std::string::npos);
}

TEST(DiagsessionImport, ReadsAllReferencedResourcesAndDiscardsPartialConversionOnError) {
    auto data = session(etl(sample(1100, 0x1010)), etl(sample(1200, 0x2020)));
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    ASSERT_EQ(model.events().size(), 2u);
    EXPECT_EQ(model.events()[0].process_instance_id, model.events()[1].process_instance_id);
    EXPECT_NE(json::parse(model.args()[model.events()[0].args_idx])["source_resource"],
              json::parse(model.args()[model.events()[1].args_idx])["source_resource"]);
    data = session(etl(sample(1100, 0x1010)), "broken");
    EXPECT_FALSE(read_diagsession(data, model, error));
    EXPECT_TRUE(model.events().empty());
    EXPECT_NE(error.find("second.etl"), std::string::npos);
    data = session(etl(sample(1100, 0x1010)));
    EXPECT_FALSE(read_diagsession(data, model, error, [](auto, float) { return false; }));
    EXPECT_EQ(error, "Import canceled");
    EXPECT_TRUE(model.events().empty());
}

TEST(DiagsessionImport, CorrelatesAcrossResourcesAndRetainsEqualTimeSamplesAndRecursion) {
    auto definitions = etl(process(3, 1010, 42) + thread(3, 1020) + module(3, 1030, "work.dll") +
                           stack(1100, {0x1010, 0x1010, 0x1020}));
    auto observations = etl(sample(1100, 0x1010) + sample(1100, 0x1010));
    auto data = session(definitions, observations);
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    ASSERT_EQ(model.events().size(), 2u);
    EXPECT_DOUBLE_EQ(model.events()[0].ts, model.events()[1].ts);
    EXPECT_EQ(model.events()[0].pid, 7u);
    EXPECT_EQ(model.events()[0].stack_frame_id, model.events()[1].stack_frame_id);
    auto frames = model.build_sample_stack(0);
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(model.stack_frames()[frames[1]].address, 0x1010);
    EXPECT_EQ(model.stack_frames()[frames[2]].address, 0x1010);
    EXPECT_NE(model.stack_frames()[frames[1]].id_idx, model.stack_frames()[frames[2]].id_idx);
    EXPECT_NE(model.stack_frames()[frames[2]].module_id, UINT32_MAX);
}

TEST(DiagsessionImport, DecodesHeapRecordsInSamePassAndDoesNotProveSurvivalAcrossUnsupportedRecords) {
    auto records = process(1, 1010, 42) + thread(1, 1020) + heap(32, 1030, 0) + heap(33, 1100, 0x5000, 100) +
                   stack(1100, {0x1010, 0x1020}) + heap(36, 1150, 0x5000) + heap(33, 1200, 0x5000, 200) +
                   stack(1200, {0x1030});
    // The metadata type selects the trace even when its name has no ETL suffix.
    auto data = session(etl(records), "", "capture.bin");
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    const auto& allocations = model.profile().allocations;
    ASSERT_EQ(allocations.size(), 2u);
    EXPECT_EQ(allocations[0].end_state, AllocationEnd::Freed);
    EXPECT_EQ(allocations[0].freed_ts, 15);
    EXPECT_EQ(allocations[1].end_state, AllocationEnd::LiveAtCaptureEnd);
    EXPECT_NE(allocations[0].id, allocations[1].id);
    EXPECT_NE(allocations[0].stack_frame_id, allocations[1].stack_frame_id);
    EXPECT_FALSE(allocations[0].stack_frame_id.empty());
    data = session(etl(records + heap(33, 1300, 0x5000, 300, 99)));
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    EXPECT_EQ(model.profile().allocations[0].end_state, AllocationEnd::Freed);
    EXPECT_EQ(model.profile().allocations[1].end_state, AllocationEnd::Unknown);
    EXPECT_TRUE(model.profile().quality.incomplete_capture);
    EXPECT_EQ(model.profile().quality.lost_events, 0);
    data = session(etl(records + manifest_heap()));
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    EXPECT_EQ(model.profile().allocations[0].end_state, AllocationEnd::Freed);
    EXPECT_EQ(model.profile().allocations[1].end_state, AllocationEnd::Unknown);
    EXPECT_TRUE(model.profile().quality.allocation_history_gaps);
    EXPECT_EQ(model.profile().quality.lost_events, 0);
    data = session(etl(manifest_heap()));
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    EXPECT_TRUE(model.events().empty());
    EXPECT_TRUE(model.profile().allocations.empty());
    EXPECT_TRUE(model.profile().quality.allocation_history_gaps);
    EXPECT_FALSE(model.profile().capabilities.native_allocation_history);
    EXPECT_NE(std::find(model.profile().quality.warnings.begin(), model.profile().quality.warnings.end(),
                        "No supported CPU samples or native allocation history were found in ETL resources"),
              model.profile().quality.warnings.end());
}

TEST(DiagsessionImport, AuthenticObsCpuCorpus) {
    const char* path = std::getenv("TRACE_NATIVE_CPU_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_NATIVE_CPU_FIXTURE to the fetched native-cpu-obs.diagsession";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    std::string data((std::istreambuf_iterator<char>(file)), {});
    TraceModel model;
    TraceParser parser;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), model)) << parser.error_message();
    ASSERT_EQ(model.events().size(), 25232u);
    size_t correlated = 0, parts = 0, mismatched_ip = 0;
    for (const auto& e : model.events()) {
        auto args = json::parse(model.args()[e.args_idx]);
        ASSERT_EQ(e.kind, EventKind::Sample);
        EXPECT_DOUBLE_EQ(e.dur, 0);
        EXPECT_EQ(args["qpc_frequency_hz"], 10000000);
        if (args.contains("raw_stack_parts")) {
            ++correlated;
            parts += args["raw_stack_parts"].size();
            bool matches_part = false;
            for (const auto& part : args["raw_stack_parts"])
                matches_part |= !part.empty() && args["raw_sample_ip"] == part[0];
            if (!matches_part) ++mismatched_ip;
        }
    }
    EXPECT_EQ(correlated, 3944u);
    EXPECT_EQ(parts, 4475u);
    EXPECT_EQ(mismatched_ip, 2u);
    auto qt = std::find_if(model.profile().modules.begin(), model.profile().modules.end(),
                           [](const auto& m) { return m.load_address == 0x7ffbd1560000ULL; });
    ASSERT_NE(qt, model.profile().modules.end());
    EXPECT_EQ(qt->size_bytes, 6471680u);
    EXPECT_EQ(qt->build_id, "1a6ed01c-6ce2-40ae-8959-60a52daebe62/1");
    EXPECT_FALSE(qt->load_ts);
    EXPECT_FALSE(qt->unload_ts);
    EXPECT_EQ(model.profile().quality.lost_events, 0);
}

TEST(DiagsessionImport, AuthenticCompoundCpuCapture) {
    const char* path = std::getenv("TRACE_COMPOUND_CPU_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_COMPOUND_CPU_FIXTURE to an authentic OLE .diagsession";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    std::string data((std::istreambuf_iterator<char>(file)), {});
    ASSERT_EQ(data.substr(0, 8), std::string("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8));
    TraceModel model;
    TraceParser parser;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), model)) << parser.error_message();
    EXPECT_EQ(model.events().size(), 69833u);
    EXPECT_TRUE(model.profile().capabilities.native_cpu_samples);
    EXPECT_TRUE(model.profile().capabilities.managed_cpu_samples);
    ASSERT_NE(model.find_process(29796), nullptr);
    EXPECT_EQ(model.find_process(29796)->name, "SimpleFunction.exe");
    bool checked = false;
    for (const auto& e : model.events()) {
        EXPECT_EQ(e.kind, EventKind::Sample);
        EXPECT_DOUBLE_EQ(e.dur, 0);
        auto args = json::parse(model.args()[e.args_idx]);
        if (args["raw_qpc"] != 332187593497ULL) continue;
        checked = true;
        EXPECT_EQ(e.pid, 29796u);
        EXPECT_EQ(e.tid, 3860u);
        auto frames = model.build_sample_stack(uint32_t(&e - model.events().data()));
        std::vector<std::string> names;
        for (auto index : frames) {
            const auto& frame = model.stack_frames()[index];
            if (frame.symbol_resolved) names.push_back(model.get_string(frame.name_idx));
            EXPECT_EQ(frame.source_line, 0u);
        }
        EXPECT_EQ(names,
                  (std::vector<std::string>{
                      "SimpleFunction.Program.Main(class System.String[])",
                      "SimpleFunction.NeuralNetwork..ctor(int32[],value class SimpleFunction.ActivationFunctions[])",
                      "SimpleFunction.NeuralNetwork.InitBiases()"}));
    }
    EXPECT_TRUE(checked);
}

TEST(DiagsessionImport, ManagedMixedRecursionAndCodeReuseSurviveReopening) {
    auto records =
        process(1, 1010, 42) + thread(1, 1020) + module(10, 1030, "native.dll") + clr_method(143, 1100, 1, "Original") +
        sample(1200, 0x2015) + stack(1200, {0x2010, 0x2010, 0x1010, 0xdead}) + clr_method(144, 1250, 1, "Original") +
        clr_method(143, 1300, 2, "Replacement") + sample(1400, 0x2010) + stack(1400, {0x2010, 0x1010, 0xdead});
    auto data = session(etl(records));
    TraceModel model;
    std::string error;
    ASSERT_TRUE(read_diagsession(data, model, error)) << error;
    ASSERT_EQ(model.events().size(), 2u);
    NativeSymbolResolver resolver;
    resolver.resolve_profile(model);
    auto first = model.build_sample_stack(0);
    auto second = model.build_sample_stack(1);
    ASSERT_EQ(first.size(), 4u);
    ASSERT_EQ(second.size(), 3u);
    EXPECT_EQ(model.get_string(model.stack_frames()[first[0]].cat_idx), "Unresolved");
    EXPECT_EQ(model.get_string(model.stack_frames()[first[1]].cat_idx), "Native");
    EXPECT_EQ(model.get_string(model.stack_frames()[first[2]].cat_idx), "Managed");
    EXPECT_EQ(model.get_string(model.stack_frames()[first[2]].name_idx), "Example.Original()");
    EXPECT_EQ(model.get_string(model.stack_frames()[second[2]].name_idx), "Example.Replacement()");
    EXPECT_NE(model.stack_frames()[first[2]].id_idx, model.stack_frames()[first[3]].id_idx);
    EXPECT_EQ(model.stack_frames()[first[2]].symbol_id, model.stack_frames()[first[3]].symbol_id);
    EXPECT_NE(model.stack_frames()[first[2]].symbol_id, model.stack_frames()[second[2]].symbol_id);
    EXPECT_TRUE(model.stack_frames()[first[2]].symbol_resolved);
    EXPECT_EQ(json::parse(model.args()[model.events()[0].args_idx])["raw_sample_ip"], 0x2015);
    std::string saved;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    TraceModel reopened;
    ASSERT_TRUE(read_profile(saved, reopened, error)) << error;
    EXPECT_TRUE(reopened.profile().capabilities.managed_cpu_samples);
    ASSERT_EQ(reopened.events().size(), model.events().size());
    for (uint32_t i = 0; i < model.events().size(); ++i) {
        auto a = model.build_sample_stack(i), b = reopened.build_sample_stack(i);
        ASSERT_EQ(a.size(), b.size());
        EXPECT_EQ(model.args()[model.events()[i].args_idx], reopened.args()[reopened.events()[i].args_idx]);
        EXPECT_EQ(reopened.events()[i].sample_weight, 1);
        EXPECT_EQ(reopened.events()[i].ts, model.events()[i].ts);
        for (size_t j = 0; j < a.size(); ++j) {
            const auto& original = model.stack_frames()[a[j]];
            const auto& restored = reopened.stack_frames()[b[j]];
            EXPECT_EQ(original.address, restored.address);
            EXPECT_EQ(original.symbol_resolved, restored.symbol_resolved);
            EXPECT_EQ(model.get_string(original.name_idx), reopened.get_string(restored.name_idx));
            EXPECT_EQ(model.get_string(original.symbol_id), reopened.get_string(restored.symbol_id));
        }
    }
}

TEST(DiagsessionImport, DesktopLoaderResolvesEmbeddedPdbWithoutExternalFiles) {
    if (!NativeSymbolResolver::available()) GTEST_SKIP() << "Optional LLVM backend is disabled";
    const auto pdb_path = std::filesystem::path(__FILE__).parent_path() / "fixtures/native_symbols/fixture.pdb";
    std::ifstream input(pdb_path, std::ios::binary);
    std::string pdb((std::istreambuf_iterator<char>(input)), {});
    ASSERT_FALSE(pdb.empty());
    auto image = module(10, 1030, "fixture.exe");
    put(image, 32 + 8, 0x4000, 8);
    std::string payload(32, '\0');
    put(payload, 0, 0x1000, 8);
    put(payload, 8, 7, 4);
    put(payload, 12, 0x5c6542d2, 4);
    put(payload, 16, 0x6c0d, 2);
    put(payload, 18, 0x3468, 2);
    put(payload, 20, 0x2e42445020444c4cULL, 8);
    put(payload, 28, 1, 4);
    payload += "fixture.pdb";
    payload += '\0';
    std::string rsds(48, '\0');
    put(rsds, 0, 48 + payload.size(), 2);
    put(rsds, 2, 20, 1);
    put(rsds, 3, 0xc0, 1);
    put(rsds, 4, 36, 1);
    put(rsds, 6, 2, 2);
    put(rsds, 8, 9, 4);
    put(rsds, 12, 7, 4);
    put(rsds, 16, 1035, 8);
    put(rsds, 24, 0xb3e675d7, 4);
    put(rsds, 28, 0x2554, 2);
    put(rsds, 30, 0x4f18, 2);
    put(rsds, 32, 0xde60257362270b83ULL, 8);
    rsds += payload;
    rsds.resize((rsds.size() + 7) & ~size_t(7));
    const auto data = session(
        etl(process(1, 1010, 42) + thread(1, 1020) + image + rsds + sample(1100, 0x2005) + stack(1100, {0x2005})), "",
        "cpu.etl", pdb);
    FileLoader loader;
    loader.load_buffer({data.begin(), data.end()}, "embedded.diagsession", false);
    loader.join();
    ASSERT_TRUE(loader.poll_finished());
    ASSERT_TRUE(loader.success()) << loader.error();
    auto model = loader.take_model();
    ASSERT_EQ(model.events().size(), 1u);
    ASSERT_EQ(model.stack_frames().size(), 1u);
    ASSERT_TRUE(model.stack_frames()[0].symbol_resolved);
    EXPECT_EQ(model.get_string(model.stack_frames()[0].name_idx), "allocate_buffer");
    EXPECT_EQ(model.stack_frames()[0].source_line, 3u);
    ASSERT_EQ(model.profile().modules.size(), 1u);
    EXPECT_EQ(model.profile().modules[0].pdb_path, "fixture.pdb");
    std::string saved, error;
    ASSERT_TRUE(serialize_profile(model, saved, error)) << error;
    ASSERT_TRUE(read_profile(saved, model, error)) << error;
    EXPECT_EQ(model.get_string(model.stack_frames()[0].name_idx), "allocate_buffer");
}

TEST(DiagsessionImport, AuthenticAzureManagedCpuAncestry) {
    const char* path = std::getenv("TRACE_MANAGED_CPU_FIXTURE");
    if (!path) GTEST_SKIP() << "Set TRACE_MANAGED_CPU_FIXTURE to cpu-azure-durabletask.diagsession";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    std::string data((std::istreambuf_iterator<char>(file)), {});
    TraceModel model;
    TraceParser parser;
    ASSERT_TRUE(parser.parse_buffer(data.data(), data.size(), model)) << parser.error_message();
    ASSERT_EQ(model.events().size(), 31664u);
    EXPECT_TRUE(model.profile().capabilities.managed_cpu_samples);
    ASSERT_NE(model.find_process(6592), nullptr);
    EXPECT_EQ(model.find_process(6592)->name, "w3wp.exe");
    size_t correlated = 0;
    bool checked = false;
    for (uint32_t i = 0; i < model.events().size(); ++i) {
        const auto& event = model.events()[i];
        auto args = json::parse(model.args()[event.args_idx]);
        correlated += args.contains("raw_stack_parts");
        EXPECT_EQ(event.kind, EventKind::Sample);
        EXPECT_EQ(event.sample_weight, 1);
        if (args["raw_qpc"] != 1127633188921ULL) continue;
        checked = true;
        EXPECT_EQ(event.pid, 6592u);
        EXPECT_EQ(event.tid, 4688u);
        EXPECT_TRUE(args.contains("thread_instance_id"));
        auto frames = model.build_sample_stack(i);
        ASSERT_EQ(frames.size(), 129u);
        // These addresses/names were checked directly against CLR verbose rundown
        // payloads and StackWalk in the published capture, before conversion.
        const std::vector<std::pair<uint64_t, std::string>> leaf = {
            {0x7ffed9ea7fcfULL,
             "DurableTask.AzureStorage.OrchestrationSessionManager+<GetNextSessionAsync>d__18.MoveNext()"},
            {0x7ffed9ea79c9ULL, "System.Runtime.CompilerServices.AsyncMethodBuilderCore.Start(!!0&)"},
            {0x7ffed9ea7950ULL,
             "DurableTask.AzureStorage.OrchestrationSessionManager.GetNextSessionAsync(value class "
             "System.Threading.CancellationToken)"},
            {0x7ffed9ea6574ULL,
             "DurableTask.AzureStorage.AzureStorageOrchestrationService+<LockNextTaskOrchestrationWorkItemAsync>d__69."
             "MoveNext()"}};
        for (size_t j = 0; j < leaf.size(); ++j) {
            const auto& frame = model.stack_frames()[frames[frames.size() - 1 - j]];
            EXPECT_EQ(frame.address, leaf[j].first);
            EXPECT_EQ(model.get_string(frame.name_idx), leaf[j].second);
            EXPECT_TRUE(frame.symbol_resolved);
        }
        const auto& raw = args["raw_stack_parts"][0];
        ASSERT_EQ(raw.size(), frames.size());
        for (size_t j = 0; j < frames.size(); ++j)
            EXPECT_EQ(model.stack_frames()[frames[j]].address, raw[raw.size() - 1 - j].get<uint64_t>());
        const auto& once = model.stack_frames()[frames[frames.size() - 4]];
        const auto& again = model.stack_frames()[frames[frames.size() - 15]];
        EXPECT_EQ(once.symbol_id, again.symbol_id);
        EXPECT_NE(once.id_idx, again.id_idx);
    }
    EXPECT_EQ(correlated, 23689u);
    EXPECT_TRUE(checked);
}
