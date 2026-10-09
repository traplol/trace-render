#include <gtest/gtest.h>
#include "parser/diagsession_container.h"
#include "parser/xpress_huffman.h"
#include <miniz.h>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
void u16(std::string& data, size_t pos, uint16_t value) {
    data.at(pos) = char(value);
    data.at(pos + 1) = char(value >> 8);
}
void u32(std::string& data, size_t pos, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(pos + i) = char(value >> (8 * i));
}
std::string metadata(const std::string& prefix = "resource-b") {
    return "<Package><Content><Resource Id='cpu' Type='DiagnosticsHub.Resource.EtlFile' Name='capture.etl' "
           "IsDirectoryOnDisk='false' ResourcePackageUriPrefix='" +
           prefix + "'/></Content><Metadata><Item Key='_BuildVersion'>17.0</Item></Metadata></Package>";
}
std::string zip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0)) return {};
    for (const auto& [name, bytes] : files)
        if (!mz_zip_writer_add_mem(&archive, name.c_str(), bytes.data(), bytes.size(), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            return {};
        }
    void* data = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &data, &size)) {
        mz_zip_writer_end(&archive);
        return {};
    }
    std::string result(static_cast<char*>(data), size);
    mz_free(data);
    mz_zip_writer_end(&archive);
    return result;
}
std::string utf16(const std::string& text) {
    std::string result((text.size() + 1) * 2, '\0');
    for (size_t i = 0; i < text.size(); ++i) result[i * 2] = text[i];
    return result;
}
std::string properties() {
    // One original OLE property set mapping a non-positional physical stream E7.
    std::string dictionary(4, '\0');
    u32(dictionary, 0, 1);
    auto name = utf16("E7");
    dictionary.resize(12);
    u32(dictionary, 4, 7);
    u32(dictionary, 8, 3);
    dictionary += name;
    dictionary.resize((dictionary.size() + 3) & ~size_t(3));
    std::string value(8, '\0');
    auto prefix = utf16("resource-b");
    u32(value, 0, 8);
    u32(value, 4, prefix.size());
    value += prefix;
    std::string result(48 + 40, '\0');
    u16(result, 0, 0xfffe);
    u32(result, 24, 1);
    u32(result, 44, 48);
    u32(result, 52, 3);
    u32(result, 56, 0);
    u32(result, 60, 40);
    u32(result, 64, 1);
    u32(result, 68, 32);
    u32(result, 72, 7);
    u32(result, 76, 40 + dictionary.size());
    u32(result, 80, 2);
    u16(result, 84, 1200);
    result += dictionary;
    result += value;
    u32(result, 48, result.size() - 48);
    return result;
}
std::string wrapped(const std::string& payload) {
    std::string result(24, '\0');
    auto crc = mz_crc32(0, reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    u32(result, 0, payload.size());
    u32(result, 4, payload.size() + 16);
    u32(result, 8, crc);
    u32(result, 12, payload.size());
    u32(result, 16, crc);
    u32(result, 20, payload.size());
    return result + payload;
}
std::string compound(const std::string& meta = metadata(), const std::string& payload = "original trace bytes") {
    const uint32_t free = 0xffffffff, end = 0xfffffffe;
    std::vector<std::pair<std::string, std::string>> streams = {
        {"metadata.xml", meta}, {"E7", wrapped(payload)}, {"\005Mgj4efc5D0geeas1LvgnikgsJe", properties()}};
    std::string minis, minifat(512, '\xff'), directory(512, '\0');
    auto entry = [&](size_t index, const std::string& name, uint8_t type, uint32_t next, uint32_t child, uint32_t start,
                     uint32_t size) {
        size_t p = index * 128;
        auto wide = utf16(name);
        directory.replace(p, wide.size(), wide);
        u16(directory, p + 64, wide.size());
        directory[p + 66] = char(type);
        directory[p + 67] = 1;
        u32(directory, p + 68, free);
        u32(directory, p + 72, next);
        u32(directory, p + 76, child);
        u32(directory, p + 116, start);
        u32(directory, p + 120, size);
    };
    for (size_t i = 0; i < streams.size(); ++i) {
        auto& [name, bytes] = streams[i];
        uint32_t start = minis.size() / 64;
        uint32_t blocks = (bytes.size() + 63) / 64;
        for (uint32_t j = 0; j < blocks; ++j) u32(minifat, (start + j) * 4, j + 1 == blocks ? end : start + j + 1);
        minis += bytes;
        minis.resize(size_t(start + blocks) * 64);
        entry(i + 1, name, 2, i + 1 < streams.size() ? i + 2 : free, free, start, bytes.size());
    }
    size_t mini_size = minis.size();
    minis.resize((minis.size() + 511) & ~size_t(511));
    uint32_t mini_sectors = minis.size() / 512, mini_fat_sector = mini_sectors + 1, fat_sector = mini_sectors + 2;
    entry(0, "Root Entry", 5, free, 1, 1, mini_size);
    std::string fat(512, '\xff');
    u32(fat, 0, end);
    for (uint32_t i = 1; i <= mini_sectors; ++i) u32(fat, i * 4, i == mini_sectors ? end : i + 1);
    u32(fat, mini_fat_sector * 4, end);
    u32(fat, fat_sector * 4, 0xfffffffd);
    std::string header(512, '\0');
    header.replace(0, 8, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8);
    u16(header, 24, 0x3e);
    u16(header, 26, 3);
    u16(header, 28, 0xfffe);
    u16(header, 30, 9);
    u16(header, 32, 6);
    u32(header, 44, 1);
    u32(header, 48, 0);
    u32(header, 56, 4096);
    u32(header, 60, mini_fat_sector);
    u32(header, 64, 1);
    u32(header, 68, end);
    for (size_t i = 0; i < 109; ++i) u32(header, 76 + i * 4, i ? free : fat_sector);
    return header + directory + minis + minifat + fat;
}
std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}  // namespace

TEST(DiagsessionContainer, ZipFollowsMetadataAndChecksReferencedContent) {
    auto data =
        zip({{"wrong/first.etl", "wrong"}, {"metadata.xml", metadata()}, {"resource-b/capture.etl", "correct"}});
    DiagsessionContainer container;
    std::string error;
    std::vector<uint8_t> resource;
    ASSERT_TRUE(container.open(data, error)) << error;
    EXPECT_EQ(container.format(), "zip");
    EXPECT_EQ(container.producer_version(), "17.0");
    ASSERT_EQ(container.resources().size(), 1u);
    EXPECT_EQ(container.resources()[0].stored_path, "resource-b/capture.etl");
    ASSERT_TRUE(container.read_resource(0, resource, error)) << error;
    EXPECT_EQ(std::string(resource.begin(), resource.end()), "correct");
    EXPECT_FALSE(container.read_resource(0, resource, error, [] { return true; }));
    EXPECT_TRUE(resource.empty());
    EXPECT_NE(error.find("cancelled"), std::string::npos);
    data = zip({{"metadata.xml", metadata("missing")}, {"resource-b/capture.etl", "correct"}});
    EXPECT_FALSE(container.open(data, error));
    EXPECT_TRUE(container.resources().empty());
}
TEST(DiagsessionContainer, CompoundMapsDictionaryAndReadsMiniStreams) {
    auto data = compound();
    DiagsessionContainer container;
    std::string error;
    std::vector<uint8_t> resource;
    ASSERT_TRUE(container.open(data, error)) << error;
    EXPECT_EQ(container.format(), "cfb");
    ASSERT_EQ(container.resources().size(), 1u);
    EXPECT_EQ(container.resources()[0].stored_path, "E7");
    ASSERT_TRUE(container.read_resource(0, resource, error)) << error;
    EXPECT_EQ(std::string(resource.begin(), resource.end()), "original trace bytes");
    auto pos = data.find("original trace bytes");
    ASSERT_NE(pos, std::string::npos);
    data[pos] ^= 1;
    ASSERT_TRUE(container.open(data, error)) << error;
    EXPECT_FALSE(container.read_resource(0, resource, error));
    EXPECT_NE(error.find("checksum"), std::string::npos);
}
TEST(DiagsessionContainer, CancelsWhileInflatingZipAndEnumeratesDirectories) {
    auto xml = metadata();
    auto flag = xml.find("IsDirectoryOnDisk='false'");
    xml.replace(flag, std::strlen("IsDirectoryOnDisk='false'"), "IsDirectoryOnDisk='true'");
    auto data = zip({{"metadata.xml", xml}, {"resource-b/sub/large.pdb", std::string(200000, 'x')}});
    DiagsessionContainer container;
    std::string error;
    std::vector<uint8_t> resource;
    ASSERT_TRUE(container.open(data, error)) << error;
    ASSERT_EQ(container.resources().size(), 2u);
    EXPECT_TRUE(container.resources()[0].directory);
    EXPECT_EQ(container.resources()[1].name, "capture.etl/sub/large.pdb");
    EXPECT_FALSE(container.read_resource(0, resource, error));
    int callbacks = 0;
    EXPECT_FALSE(container.read_resource(1, resource, error, [&] { return ++callbacks >= 3; }));
    EXPECT_GE(callbacks, 3);
    EXPECT_TRUE(resource.empty());
    EXPECT_NE(error.find("cancelled"), std::string::npos);
    data = zip({{"metadata.xml", xml}, {"resource-b/", ""}});
    ASSERT_TRUE(container.open(data, error)) << error;
    ASSERT_EQ(container.resources().size(), 1u);
}
TEST(DiagsessionContainer, RejectsMalformedCompoundAndXml) {
    DiagsessionContainer container;
    std::string error;
    auto missing = compound(metadata("missing"));
    EXPECT_FALSE(container.open(missing, error));
    auto cyclic = compound();
    u32(cyclic, 512 + 128 + 72, 1);
    EXPECT_FALSE(container.open(cyclic, error));
    EXPECT_NE(error.find("directory tree"), std::string::npos);
    auto truncated = compound();
    truncated.pop_back();
    EXPECT_FALSE(container.open(truncated, error));
    auto bad_fat = compound();
    u32(bad_fat, 76, 0x12345678);
    EXPECT_FALSE(container.open(bad_fat, error));
    auto bad_xml = zip({{"metadata.xml", "<Package>"}});
    EXPECT_FALSE(container.open(bad_xml, error));
    EXPECT_FALSE(container.open("arbitrary data", error));
}
TEST(XpressHuffman, LiteralTableAndCorruption) {
    // A complete fixed 8-bit alphabet for literals0..255, all match symbols unused.
    std::string data(256, '\0');
    std::fill_n(data.begin(), 128, char(0x88));
    // The bit reader consumes little-endian words, high word first.
    data += std::string("BA\0\0\0\0", 6);
    std::vector<uint8_t> output;
    std::string error;
    ASSERT_TRUE(decompress_xpress_huffman(data, 2, output, error)) << error;
    EXPECT_EQ(std::string(output.begin(), output.end()), "AB");
    data[0] = 0;
    EXPECT_FALSE(decompress_xpress_huffman(data, 2, output, error));
    EXPECT_TRUE(output.empty());
    EXPECT_FALSE(decompress_xpress_huffman("", 1, output, error));
}
TEST(DiagsessionContainer, AuthenticZipAndCompoundResources) {
    const char* directory = std::getenv("TRACE_RENDER_DIAGSESSION_CORPUS");
    if (!directory) GTEST_SKIP() << "Set TRACE_RENDER_DIAGSESSION_CORPUS to the downloaded corpus directory";
    for (const char* name : {"native-cpu-obs.diagsession", "cpu-perfview-compound.diagsession"}) {
        auto data = read_file(std::string(directory) + "/" + name);
        ASSERT_FALSE(data.empty()) << name;
        DiagsessionContainer container;
        std::string error;
        ASSERT_TRUE(container.open(data, error)) << name << ": " << error;
        size_t etls = 0;
        for (size_t i = 0; i < container.resources().size(); ++i) {
            const auto& resource = container.resources()[i];
            if (resource.directory) continue;
            std::vector<uint8_t> output;
            ASSERT_TRUE(container.read_resource(i, output, error))
                << name << "/" << resource.stored_path << ": " << error;
            ASSERT_FALSE(output.empty());
            if (resource.type == "DiagnosticsHub.Resource.EtlFile") {
                ++etls;
                EXPECT_GT(output.size(), 100000u);
            }
        }
        EXPECT_GT(etls, 0u);
    }
}
