#include "diagsession_container.h"
#include "etl_reader.h"
#include "xpress_huffman.h"
#include <miniz.h>
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
constexpr uint32_t end_chain = 0xfffffffe;
constexpr uint32_t free_sector = 0xffffffff;
constexpr size_t resource_limit = size_t(1) << 31;
const std::string root_properties = "\005Mgj4efc5D0geeas1LvgnikgsJe";
const std::string child_properties = "\005Lqygmiv0W3brumg4Xgruuwxc3h";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string_view view(const std::vector<uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string joined(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

class CompoundFile {
public:
    struct Entry {
        std::string name;
        uint8_t type;
        uint32_t left, right, child, start;
        uint64_t size;
    };
    explicit CompoundFile(std::string_view bytes) : bytes_(bytes) {
        EtlBytes header(bytes);
        require(bytes.size() >= 512, "truncated compound header");
        auto major = header.u16(26);
        require(header.u16(28) == 0xfffe &&
                    ((major == 3 && header.u16(30) == 9) || (major == 4 && header.u16(30) == 12)) &&
                    header.u16(32) == 6 && header.u32(56) == 4096,
                "unsupported compound-file sector format");
        sector_size_ = size_t(1) << header.u16(30);
        require(bytes.size() >= sector_size_ && bytes.size() % sector_size_ == 0, "truncated compound-file sector");
        sector_count_ = bytes.size() / sector_size_ - 1;
        uint32_t fat_count = header.u32(44);
        require(fat_count <= sector_count_, "invalid FAT size");
        std::vector<uint32_t> fat_sectors;
        auto add_fat = [&](uint32_t id) {
            if (id != free_sector) fat_sectors.push_back(id);
        };
        for (size_t i = 0; i < 109; ++i) add_fat(header.u32(76 + i * 4));
        uint32_t difat = header.u32(68), difat_count = header.u32(72);
        require(difat_count <= sector_count_, "invalid DIFAT size");
        std::set<uint32_t> seen;
        for (uint32_t i = 0; i < difat_count; ++i) {
            require(seen.insert(difat).second, "cyclic DIFAT");
            EtlBytes data(sector(difat));
            for (size_t n = 0; n < sector_size_ / 4 - 1; ++n) add_fat(data.u32(n * 4));
            difat = data.u32(sector_size_ - 4);
        }
        require(difat_count == 0 || difat == end_chain, "invalid DIFAT terminator");
        require(fat_sectors.size() == fat_count, "FAT count does not match header");
        seen.clear();
        for (uint32_t id : fat_sectors) {
            require(seen.insert(id).second, "duplicate FAT sector");
            EtlBytes data(sector(id));
            for (size_t n = 0; n < sector_size_; n += 4) fat_.push_back(data.u32(n));
        }
        auto directory = chain(header.u32(48), 0, false);
        require(!directory.empty() && directory.size() % 128 == 0, "invalid compound directory");
        for (size_t pos = 0; pos < directory.size(); pos += 128) {
            EtlBytes data(view(directory).substr(pos, 128));
            Entry entry{};
            entry.type = data.u8(66);
            if (entry.type) {
                auto length = data.u16(64);
                require(length >= 2 && length <= 64 && length % 2 == 0, "invalid compound entry name");
                entry.name = EtlBytes(data.slice(0, length)).utf16(0);
                require(entry.name.find('/') == std::string::npos && entry.name.find('\\') == std::string::npos,
                        "invalid compound path component");
                entry.left = data.u32(68);
                entry.right = data.u32(72);
                entry.child = data.u32(76);
                entry.start = data.u32(116);
                entry.size = data.u64(120);
                if (major == 3) require(entry.size <= UINT32_MAX, "invalid version-3 stream size");
            }
            entries_.push_back(std::move(entry));
        }
        require(entries_[0].type == 5, "missing compound root storage");
        mini_stream_ = chain(entries_[0].start, entries_[0].size, true);
        uint32_t mini_count = header.u32(64);
        require(mini_count <= sector_count_, "invalid mini FAT size");
        auto mini_fat = chain(header.u32(60), uint64_t(mini_count) * sector_size_, true);
        EtlBytes mini_data(view(mini_fat));
        for (size_t i = 0; i < mini_fat.size(); i += 4) mini_fat_.push_back(mini_data.u32(i));
        seen.clear();
        seen.insert(0);
        add_entries(entries_[0].child, "", seen, 0);
    }
    const std::map<std::string, size_t>& paths() const { return paths_; }
    bool directory(const std::string& path) const { return entries_.at(paths_.at(path)).type == 1; }
    std::vector<uint8_t> read(const std::string& path) const {
        auto found = paths_.find(path);
        require(found != paths_.end(), "compound resource is missing");
        const auto& entry = entries_[found->second];
        require(entry.type == 2, "compound resource is not a stream");
        if (entry.size >= 4096) return chain(entry.start, entry.size, true);
        std::vector<uint8_t> output;
        uint32_t id = entry.start;
        while (output.size() < entry.size) {
            require(id < mini_fat_.size(), "invalid mini stream chain");
            size_t offset = size_t(id) * 64;
            require(offset <= mini_stream_.size() && mini_stream_.size() - offset >= 64,
                    "mini stream lies outside root stream");
            size_t count = std::min<size_t>(64, entry.size - output.size());
            output.insert(output.end(), mini_stream_.begin() + offset, mini_stream_.begin() + offset + count);
            id = mini_fat_[id];
        }
        require(entry.size == 0 || id == end_chain, "cyclic or oversized mini stream chain");
        return output;
    }

private:
    std::string_view bytes_;
    size_t sector_size_ = 0, sector_count_ = 0;
    std::vector<uint32_t> fat_, mini_fat_;
    std::vector<uint8_t> mini_stream_;
    std::vector<Entry> entries_;
    std::map<std::string, size_t> paths_;
    std::string_view sector(uint32_t id) const {
        require(id < sector_count_, "compound sector is outside file");
        return bytes_.substr((size_t(id) + 1) * sector_size_, sector_size_);
    }
    std::vector<uint8_t> chain(uint32_t id, uint64_t size, bool sized) const {
        require(size <= resource_limit && size <= bytes_.size(),
                "compound stream exceeds available data or 2 GiB limit");
        std::vector<uint8_t> output;
        size_t sectors = 0;
        while (id != end_chain && (!sized || output.size() < size)) {
            require(id < fat_.size() && ++sectors <= sector_count_, "invalid or cyclic FAT chain");
            auto data = sector(id);
            size_t count = sized ? std::min<uint64_t>(data.size(), size - output.size()) : data.size();
            require(count <= resource_limit - output.size(), "compound stream exceeds 2 GiB limit");
            output.insert(output.end(), data.begin(), data.begin() + count);
            id = fat_[id];
        }
        require(!sized || output.size() == size, "truncated compound stream");
        require(size == 0 && sized ? true : id == end_chain, "oversized compound stream chain");
        return output;
    }
    void add_entries(uint32_t id, const std::string& parent, std::set<uint32_t>& seen, size_t depth) {
        if (id == free_sector) return;
        require(id < entries_.size() && seen.insert(id).second && depth < 128, "invalid compound directory tree");
        const auto& entry = entries_[id];
        require(entry.type == 1 || entry.type == 2, "invalid compound directory entry type");
        add_entries(entry.left, parent, seen, depth + 1);
        auto path = joined(parent, entry.name);
        require(paths_.emplace(path, id).second, "duplicate compound resource path");
        if (entry.type == 1) add_entries(entry.child, path, seen, depth + 1);
        add_entries(entry.right, parent, seen, depth + 1);
    }
};

// DiagnosticsHub uses Unicode OLE property dictionaries to map E/F/D names to resource names.
std::map<std::string, std::string> property_names(std::string_view bytes) {
    EtlBytes data(bytes);
    require(data.u16(0) == 0xfffe && data.u32(24) == 1, "unsupported compound property set");
    size_t section_offset = data.u32(44);
    EtlBytes section(data.slice(section_offset, data.u32(section_offset)));
    uint32_t count = section.u32(4);
    require(count <= bytes.size() / 8, "invalid compound property count");
    std::map<uint32_t, uint32_t> offsets;
    for (uint32_t i = 0; i < count; ++i)
        require(offsets.emplace(section.u32(8 + i * 8), section.u32(12 + i * 8)).second,
                "duplicate compound property ID");
    require(offsets.count(0) && offsets.count(1), "missing compound property dictionary");
    require(section.u32(offsets.at(1)) == 2 && section.u16(offsets.at(1) + 4) == 1200,
            "unsupported compound property code page");
    size_t pos = offsets.at(0);
    uint32_t names = section.u32(pos);
    pos += 4;
    require(names <= count, "invalid compound dictionary size");
    std::map<std::string, std::string> result;
    for (uint32_t i = 0; i < names; ++i) {
        uint32_t id = section.u32(pos), length = section.u32(pos + 4);
        pos += 8;
        require(length > 0 && length <= bytes.size() / 2, "invalid compound dictionary name");
        auto name = EtlBytes(section.slice(pos, size_t(length) * 2)).utf16(0);
        pos = (pos + size_t(length) * 2 + 3) & ~size_t(3);
        auto found = offsets.find(id);
        if (found == offsets.end() || section.u32(found->second) != 8) continue;
        size_t length_bytes = section.u32(found->second + 4);
        require(length_bytes >= 2 && length_bytes % 2 == 0, "invalid compound property string");
        auto value = EtlBytes(section.slice(found->second + 8, length_bytes)).utf16(0);
        require(result.emplace(std::move(name), std::move(value)).second, "duplicate compound dictionary name");
    }
    return result;
}

std::vector<uint8_t> decompress_resource(std::string_view bytes, const std::function<bool()>& cancelled) {
    EtlBytes data(bytes);
    std::vector<uint8_t> result;
    size_t pos = 0;
    while (pos < bytes.size()) {
        if (cancelled && cancelled()) throw std::runtime_error("import cancelled");
        size_t raw_size = data.u32(pos), stored_size = data.u32(pos + 4);
        pos += 8;
        require(raw_size <= resource_limit - result.size(), "resource exceeds 2 GiB limit");
        data.slice(pos, stored_size);
        size_t end = pos + stored_size, raw_end = result.size() + raw_size;
        while (pos < end) {
            require(end - pos >= 16, "truncated resource compression header");
            uint32_t raw_crc = data.u32(pos), chunk_size = data.u32(pos + 4);
            uint32_t stored_crc = data.u32(pos + 8), compressed_size = data.u32(pos + 12);
            pos += 16;
            require(chunk_size > 0 && chunk_size <= 65536 && chunk_size <= raw_end - result.size() &&
                        compressed_size <= end - pos,
                    "invalid resource compression chunk size");
            auto compressed = data.slice(pos, compressed_size);
            pos += compressed_size;
            require(
                mz_crc32(0, reinterpret_cast<const unsigned char*>(compressed.data()), compressed.size()) == stored_crc,
                "resource compressed checksum mismatch");
            std::vector<uint8_t> raw;
            if (compressed_size == chunk_size)
                raw.assign(compressed.begin(), compressed.end());
            else {
                std::string error;
                if (!decompress_xpress_huffman(compressed, chunk_size, raw, error)) throw std::runtime_error(error);
            }
            require(mz_crc32(0, raw.data(), raw.size()) == raw_crc, "resource decompressed checksum mismatch");
            result.insert(result.end(), raw.begin(), raw.end());
        }
        require(result.size() == raw_end, "truncated decompressed resource block");
    }
    return result;
}
const char* local_name(const char* name) {
    const char* colon = std::strchr(name, ':');
    return colon ? colon + 1 : name;
}
const tinyxml2::XMLElement* child(const tinyxml2::XMLElement* parent, const char* name) {
    if (parent)
        for (auto e = parent->FirstChildElement(); e; e = e->NextSiblingElement())
            if (std::string_view(local_name(e->Name())) == name) return e;
    return nullptr;
}
std::string attribute(const tinyxml2::XMLElement* element, const char* name) {
    const char* value = element->Attribute(name);
    require(value && *value, "missing resource metadata attribute");
    return value;
}
}  // namespace

struct DiagsessionContainer::Impl {
    std::string format, producer_version;
    std::vector<DiagsessionResource> resources;
    mz_zip_archive zip{};
    std::map<std::string, uint32_t> zip_files;
    std::unique_ptr<CompoundFile> compound;
    ~Impl() {
        if (zip.m_pState) mz_zip_reader_end(&zip);
    }
    std::vector<uint8_t> read(const std::string& path, const std::function<bool()>& cancelled = {}) const {
        if (compound) return compound->read(path);
        auto found = zip_files.find(path);
        require(found != zip_files.end(), "ZIP resource is missing");
        mz_zip_archive_file_stat stat{};
        auto archive = const_cast<mz_zip_archive*>(&zip);
        require(mz_zip_reader_file_stat(archive, found->second, &stat), "invalid ZIP resource");
        require(stat.m_uncomp_size <= resource_limit, "ZIP resource exceeds 2 GiB limit");
        std::vector<uint8_t> output(static_cast<size_t>(stat.m_uncomp_size));
        struct WriteContext {
            std::vector<uint8_t>& output;
            const std::function<bool()>& cancelled;
            bool stopped = false;
        } context{output, cancelled};
        bool success = mz_zip_reader_extract_to_callback(
            archive, found->second,
            [](void* opaque, mz_uint64 offset, const void* bytes, size_t size) -> size_t {
                auto& context = *static_cast<WriteContext*>(opaque);
                if (context.cancelled && context.cancelled()) {
                    context.stopped = true;
                    return 0;
                }
                if (offset > context.output.size() || size > context.output.size() - offset) return 0;
                if (size) std::memcpy(context.output.data() + offset, bytes, size);
                return size;
            },
            &context, 0);
        require(!context.stopped, "import cancelled");
        require(success, "ZIP decompression or checksum failed");
        return output;
    }
    std::string zip_path(const std::string& expected) const {
        if (zip_files.count(expected)) return expected;
        std::string found;
        auto key = lower(expected);
        for (const auto& [path, index] : zip_files)
            if (lower(path) == key) {
                require(found.empty(), "ambiguous ZIP resource path");
                found = path;
            }
        require(!found.empty(), "metadata references a missing ZIP resource");
        return found;
    }
    void add_compound_directory(const DiagsessionResource& root, const std::string& physical,
                                const std::string& logical, size_t depth) {
        require(depth < 128, "compound resource directory is too deep");
        auto properties = compound->read(joined(physical, child_properties));
        for (const auto& [entry, name] : property_names(view(properties))) {
            auto path = joined(physical, entry), label = joined(logical, name);
            bool directory = compound->directory(path);
            resources.push_back({root.id, root.type, label, path, directory});
            if (directory) add_compound_directory(root, path, label, depth + 1);
        }
    }
    void parse_metadata() {
        auto metadata = read(compound ? "metadata.xml" : zip_path("metadata.xml"));
        tinyxml2::XMLDocument document;
        require(
            document.Parse(reinterpret_cast<const char*>(metadata.data()), metadata.size()) == tinyxml2::XML_SUCCESS,
            "invalid metadata.xml");
        auto package = document.RootElement();
        require(package && std::string_view(local_name(package->Name())) == "Package", "missing metadata Package");
        auto content = child(package, "Content");
        require(content, "missing metadata Content");
        std::map<std::string, std::string> prefixes;
        if (compound) {
            auto properties = compound->read(root_properties);
            for (const auto& [physical, prefix] : property_names(view(properties)))
                require(prefixes.emplace(lower(prefix), physical).second, "duplicate compound resource prefix");
        }
        for (auto element = content->FirstChildElement(); element; element = element->NextSiblingElement()) {
            if (std::string_view(local_name(element->Name())) != "Resource") continue;
            DiagsessionResource resource;
            resource.id = attribute(element, "Id");
            resource.type = attribute(element, "Type");
            resource.name = attribute(element, "Name");
            auto prefix = attribute(element, "ResourcePackageUriPrefix");
            auto directory = lower(attribute(element, "IsDirectoryOnDisk"));
            require(directory == "true" || directory == "false", "invalid resource directory flag");
            resource.directory = directory == "true";
            if (compound) {
                auto found = prefixes.find(lower(prefix));
                require(found != prefixes.end(), "metadata references a missing compound resource");
                resource.stored_path = found->second;
                require(compound->directory(resource.stored_path) == resource.directory,
                        "compound resource type disagrees with metadata");
                resources.push_back(resource);
                if (resource.directory) add_compound_directory(resource, resource.stored_path, resource.name, 0);
            } else if (!resource.directory) {
                resource.stored_path = zip_path(joined(prefix, resource.name));
                resources.push_back(resource);
            } else {
                resource.stored_path = prefix + "/";
                resources.push_back(resource);
                auto directory_prefix = lower(resource.stored_path);
                bool found = zip_files.count(resource.stored_path) != 0;
                for (const auto& [path, index] : zip_files) {
                    if (lower(path).compare(0, directory_prefix.size(), directory_prefix) != 0 || path.back() == '/')
                        continue;
                    resources.push_back({resource.id, resource.type,
                                         joined(resource.name, path.substr(directory_prefix.size())), path, false});
                    found = true;
                }
                require(found, "metadata references an empty or missing ZIP resource directory");
            }
        }
        if (auto metadata_element = child(package, "Metadata"))
            for (auto item = metadata_element->FirstChildElement(); item; item = item->NextSiblingElement())
                if (item->Attribute("Key", "_BuildVersion") && item->GetText()) producer_version = item->GetText();
    }
};

DiagsessionContainer::DiagsessionContainer() : impl_(std::make_unique<Impl>()) {}
DiagsessionContainer::~DiagsessionContainer() = default;
bool DiagsessionContainer::open(std::string_view bytes, std::string& error) {
    impl_ = std::make_unique<Impl>();
    error.clear();
    try {
        if (bytes.substr(0, 8) == std::string_view("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) {
            impl_->format = "cfb";
            impl_->compound = std::make_unique<CompoundFile>(bytes);
        } else if (bytes.substr(0, 2) == "PK") {
            impl_->format = "zip";
            require(mz_zip_reader_init_mem(&impl_->zip, bytes.data(), bytes.size(), 0), "invalid ZIP container");
            auto count = mz_zip_reader_get_num_files(&impl_->zip);
            for (uint32_t i = 0; i < count; ++i) {
                size_t size = mz_zip_reader_get_filename(&impl_->zip, i, nullptr, 0);
                require(size > 1, "invalid ZIP resource name");
                std::string path(size, '\0');
                require(mz_zip_reader_get_filename(&impl_->zip, i, path.data(), size) == size,
                        "invalid ZIP resource name");
                path.pop_back();
                require(impl_->zip_files.emplace(std::move(path), i).second, "duplicate ZIP resource path");
            }
        } else
            throw std::runtime_error("unsupported container signature");
        impl_->parse_metadata();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid .diagsession: ") + exception.what();
        impl_ = std::make_unique<Impl>();
        return false;
    }
}
const std::vector<DiagsessionResource>& DiagsessionContainer::resources() const {
    return impl_->resources;
}
const std::string& DiagsessionContainer::format() const {
    return impl_->format;
}
const std::string& DiagsessionContainer::producer_version() const {
    return impl_->producer_version;
}
bool DiagsessionContainer::read_resource(size_t index, std::vector<uint8_t>& output, std::string& error,
                                         std::function<bool()> cancelled) const {
    output.clear();
    error.clear();
    try {
        require(index < impl_->resources.size(), "resource index is outside metadata");
        const auto& resource = impl_->resources[index];
        require(!resource.directory, "resource is a directory");
        if (cancelled && cancelled()) throw std::runtime_error("import cancelled");
        auto stored = impl_->read(resource.stored_path, cancelled);
        output = impl_->compound ? decompress_resource(view(stored), cancelled) : std::move(stored);
        if (cancelled && cancelled()) throw std::runtime_error("import cancelled");
        return true;
    } catch (const std::exception& exception) {
        output.clear();
        error = std::string("Cannot read .diagsession resource: ") + exception.what();
        return false;
    }
}
