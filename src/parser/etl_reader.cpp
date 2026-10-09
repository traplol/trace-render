#include "etl_reader.h"
#include "xpress_huffman.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

// ETL layout references: Windows WDK ntwmi.h and the MIT-licensed Snail reader,
// https://github.com/albertziegenhagel/snail-server/tree/71b01259fff9e1ef347a77a7c77d90e35699e1b4/snail/etl
// Reads here deliberately check bounds before every access, including release builds.

std::string_view EtlBytes::slice(size_t offset, size_t size) const {
    if (offset > bytes_.size() || size > bytes_.size() - offset)
        throw std::runtime_error("Truncated ETL header or event payload");
    return bytes_.substr(offset, size);
}
uint8_t EtlBytes::u8(size_t offset) const {
    return static_cast<uint8_t>(slice(offset, 1)[0]);
}
uint16_t EtlBytes::u16(size_t offset) const {
    auto s = slice(offset, 2);
    return uint8_t(s[0]) | (uint16_t(uint8_t(s[1])) << 8);
}
uint32_t EtlBytes::u32(size_t offset) const {
    return uint32_t(u16(offset)) | (uint32_t(u16(offset + 2)) << 16);
}
uint64_t EtlBytes::u64(size_t offset) const {
    return uint64_t(u32(offset)) | (uint64_t(u32(offset + 4)) << 32);
}
uint64_t EtlBytes::pointer(size_t offset, uint32_t width) const {
    if (width == 4) return u32(offset);
    if (width == 8) return u64(offset);
    throw std::runtime_error("Unsupported ETL pointer size");
}
std::string EtlBytes::utf8(size_t offset) const {
    slice(offset, 0);
    auto end = bytes_.find('\0', offset);
    if (end == std::string_view::npos) throw std::runtime_error("Unterminated ETL string");
    return std::string(bytes_.substr(offset, end - offset));
}
std::string EtlBytes::utf16(size_t offset) const {
    std::string result;
    while (true) {
        uint32_t c = u16(offset);
        offset += 2;
        if (!c) return result;
        if (c >= 0xd800 && c <= 0xdbff) {
            uint32_t low = u16(offset);
            offset += 2;
            if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("Invalid ETL UTF-16 surrogate");
            c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            throw std::runtime_error("Invalid ETL UTF-16 surrogate");
        }
        if (c < 0x80)
            result += char(c);
        else if (c < 0x800) {
            result += char(0xc0 | (c >> 6));
            result += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            result += char(0xe0 | (c >> 12));
            result += char(0x80 | ((c >> 6) & 63));
            result += char(0x80 | (c & 63));
        } else {
            result += char(0xf0 | (c >> 18));
            result += char(0x80 | ((c >> 12) & 63));
            result += char(0x80 | ((c >> 6) & 63));
            result += char(0x80 | (c & 63));
        }
    }
}
std::string EtlBytes::guid(size_t offset) const {
    char result[37];
    std::snprintf(result, sizeof(result), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", u32(offset),
                  u16(offset + 4), u16(offset + 6), u8(offset + 8), u8(offset + 9), u8(offset + 10), u8(offset + 11),
                  u8(offset + 12), u8(offset + 13), u8(offset + 14), u8(offset + 15));
    return result;
}

namespace {
void extensions(EtlRecord& out) {
    // On disk, the 8-byte prefix is followed by inline data, not DataPtr.
    // evntcons.h defines Linkage (another item), types 5/6, and stack layouts.
    // Packed sizes/alignment: Geoff Chappell's EVENT_HEADER study, checked
    // against the original managed-allocation fixture; see diagsession-import.md.
    EtlBytes b(out.payload);
    size_t at = 0;
    uint16_t linkage;
    do {
        size_t size = b.u16(at), data_size = b.u16(at + 6);
        linkage = b.u16(at + 4);
        if (linkage & ~1u) throw std::runtime_error("Unsupported ETL extension flags");
        if (size != ((8 + data_size + 7) & ~size_t(7))) throw std::runtime_error("Invalid ETL extension size");
        EtlExtension item;
        item.type = b.u16(at + 2);
        item.data = b.slice(at + 8, data_size);
        if (item.type == 5 || item.type == 6) {
            uint8_t width = item.type == 5 ? 4 : 8;
            if (data_size < 8 || (data_size - 8) % width) throw std::runtime_error("Invalid ETL extended stack size");
            EtlBytes data(item.data);
            item.stack = EtlExtendedStack{data.u64(0), width, {}};
            for (size_t offset = 8; offset < data_size; offset += width)
                item.stack->addresses.push_back(data.pointer(offset, width));
        }
        out.extensions.push_back(std::move(item));
        b.slice(at, size);  // Total size includes padding, which must fit this event too.
        at += size;
    } while (linkage);
    out.payload.remove_prefix(at);
}

size_t record(std::string_view bytes, EtlRecord& out) {
    EtlBytes b(bytes);
    uint8_t type = b.u8(2);
    if ((b.u8(3) & 0xc0) != 0xc0 || (b.u8(3) & 0x10)) throw std::runtime_error("Unsupported ETL record marker");
    size_t header_size = 0, size = 0;
    switch (type) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 16:
        case 17: {
            out.pointer_size = type == 1 || type == 3 || type == 16 ? 4 : 8;
            size = b.u16(4);
            out.opcode = b.u8(6);
            out.group = b.u8(7);
            out.version = b.u16(0);
            if (type == 16 || type == 17) {
                header_size = 16 + ((out.version & 0x8000) ? 8 : 0) + ((out.version & 0x700) >> 8) * 8;
                out.version &= ~0x8700;
                out.qpc = b.u64(8);
            } else {
                header_size = type == 1 || type == 2 ? 32 : 24;
                out.tid = b.u32(8);
                out.pid = b.u32(12);
                out.qpc = b.u64(16);
            }
            break;
        }
        case 10:
        case 11:
        case 18:
        case 19:
        case 20:
        case 21:
            out.pointer_size = type == 10 || type == 11 || type == 18 ? 4 : 8;
            size = b.u16(0);
            out.tid = b.u32(8);
            out.pid = b.u32(12);
            out.qpc = b.u64(16);
            out.provider = b.guid(24);
            if (type == 18 || type == 19) {
                header_size = 80;
                out.extended_data = (b.u16(4) & 1) != 0;
                out.event_id = b.u16(40);
                out.version = b.u8(42);
                out.opcode = b.u8(45);
            } else {
                header_size = type == 11 || type == 21 ? 72 : 48;
                out.opcode = b.u8(4);
                out.version = b.u16(6);
            }
            break;
        default:
            throw std::runtime_error("Unsupported ETL record header type " + std::to_string(type));
    }
    if (size < header_size) throw std::runtime_error("ETL record size is smaller than its header");
    out.payload = b.slice(header_size, size - header_size);
    if (out.extended_data) extensions(out);
    return size;
}

// MS-XCA section 2.1.4, plain LZ77 (XPRESS). Output size comes from the ETL
// buffer header; every literal and match must fit it exactly.
std::vector<char> xpress(std::string_view input, size_t output_size) {
    EtlBytes b(input);
    std::vector<char> out;
    out.reserve(output_size);
    size_t at = 0, nibble = 0;
    uint32_t flags = 0;
    int bits = 0;
    while (out.size() < output_size) {
        if (!bits) {
            flags = b.u32(at);
            at += 4;
            bits = 32;
        }
        if ((flags & (uint32_t(1) << --bits)) == 0) {
            out.push_back(char(b.u8(at++)));
            continue;
        }
        auto token = b.u16(at);
        at += 2;
        size_t distance = (token >> 3) + 1;
        uint64_t length = token & 7;
        if (length == 7) {
            if (!nibble) {
                nibble = at;
                length = b.u8(at++) & 15;
            } else {
                length = b.u8(nibble) >> 4;
                nibble = 0;
            }
            if (length == 15) {
                length = b.u8(at++);
                if (length == 255) {
                    length = b.u16(at);
                    at += 2;
                    if (!length) {
                        length = b.u32(at);
                        at += 4;
                    }
                    if (length < 22) throw std::runtime_error("Invalid XPRESS match length");
                    length -= 22;
                }
                length += 15;
            }
            length += 7;
        }
        length += 3;
        if (distance > out.size() || length > output_size - out.size())
            throw std::runtime_error("XPRESS match exceeds its buffer");
        while (length--) out.push_back(out[out.size() - distance]);
    }
    return out;
}
}  // namespace

bool read_etl(std::string_view bytes, EtlFileInfo& info, const EtlRecordCallback& consume, std::string& error,
              const ImportProgress& progress) {
    info = {};
    error.clear();
    try {
        EtlBytes file(bytes);
        if (file.u16(54) != 4) throw std::runtime_error("ETL does not begin with a header buffer");
        uint32_t first_size = file.u32(0), first_end = file.u32(4);
        if (first_size < 72 || first_end < 72 || first_end > first_size || first_size > bytes.size())
            throw std::runtime_error("Invalid ETL header buffer size");
        EtlRecord header;
        record(file.slice(72, first_end - 72), header);
        if (header.group != 0 || header.opcode != 0 || header.version != 2)
            throw std::runtime_error("Unsupported ETL logfile header");
        EtlBytes h(header.payload);
        info.pointer_size = h.u32(44);
        if (info.pointer_size != 4 && info.pointer_size != 8) throw std::runtime_error("Unsupported ETL pointer size");
        info.boot_filetime = h.u64(232 + 2 * info.pointer_size);
        info.qpc_frequency = h.u64(240 + 2 * info.pointer_size);
        info.start_qpc = header.qpc;
        info.start_filetime = h.u64(248 + 2 * info.pointer_size);
        info.end_filetime = h.u64(16);
        info.buffers_written = h.u32(36);
        info.lost_events = h.u32(48);
        info.lost_buffers = h.u32(260 + 2 * info.pointer_size);
        uint32_t clock = h.u32(256 + 2 * info.pointer_size);
        if (clock != 1 || !info.qpc_frequency)
            throw std::runtime_error("Unsupported ETL timestamp clock; QPC is required");
        uint16_t compression = (h.u32(32) & 0x4000000) ? file.u32(44) & 0xffff : 0;
        size_t offset = 0;
        uint32_t buffer_count = 0;
        while (offset < bytes.size()) {
            if (progress && !progress("Reading ETL", float(offset) / float(bytes.size())))
                throw std::runtime_error("Import canceled");
            EtlBytes bh(file.slice(offset, 72));
            uint32_t size = bh.u32(0), end = bh.u32(4);
            if (size < 72 || size > bytes.size() - offset || end < 72 || end > 64 * 1024 * 1024)
                throw std::runtime_error("Invalid or truncated ETL buffer size");
            auto body = file.slice(offset + 72, size - 72);
            std::vector<char> decoded;
            std::vector<uint8_t> huffman;
            uint16_t flags = bh.u16(52);
            if (flags & 0x0e) info.buffer_loss_flag = true;
            if (flags & 0x40) {
                if (compression == 3) {
                    decoded = xpress(body, end - 72);
                    body = {decoded.data(), decoded.size()};
                } else if (compression == 4) {
                    std::string detail;
                    if (!decompress_xpress_huffman(body, end - 72, huffman, detail))
                        throw std::runtime_error("Invalid ETL XPRESS-Huffman buffer: " + detail);
                    body = {reinterpret_cast<const char*>(huffman.data()), huffman.size()};
                } else {
                    throw std::runtime_error("Unsupported ETL buffer compression " + std::to_string(compression));
                }
            } else {
                if (end > size) throw std::runtime_error("ETL used buffer size exceeds its storage");
                body = body.substr(0, end - 72);
            }
            size_t at = 0;
            while (at < body.size()) {
                EtlRecord event;
                size_t length = record(body.substr(at), event);
                ++info.records_read;
                if (consume && !consume(info, event)) throw std::runtime_error("Import canceled");
                size_t aligned = (length + 7) & ~size_t(7);
                if (aligned > body.size() - at && length != body.size() - at)
                    throw std::runtime_error("Truncated ETL record alignment");
                at += aligned;
            }
            ++buffer_count;
            offset += size;
        }
        if (buffer_count != info.buffers_written)
            throw std::runtime_error("ETL buffer count does not match the logfile header");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
