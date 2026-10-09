#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Checked little-endian access for ETL headers and provider payloads. Invalid
// offsets and unterminated strings throw; read_etl turns these into an error.
class EtlBytes {
public:
    explicit EtlBytes(std::string_view bytes) : bytes_(bytes) {}
    uint8_t u8(size_t offset) const;
    uint16_t u16(size_t offset) const;
    uint32_t u32(size_t offset) const;
    uint64_t u64(size_t offset) const;
    uint64_t pointer(size_t offset, uint32_t width) const;
    std::string_view slice(size_t offset, size_t size) const;
    std::string utf8(size_t offset) const;
    std::string utf16(size_t offset) const;
    std::string guid(size_t offset) const;

private:
    std::string_view bytes_;
};

struct EtlFileInfo {
    uint32_t pointer_size = 0;
    uint64_t qpc_frequency = 0;
    uint64_t start_qpc = 0;
    uint64_t boot_filetime = 0;
    uint64_t start_filetime = 0;
    uint64_t end_filetime = 0;
    uint32_t buffers_written = 0;
    uint32_t lost_events = 0;
    uint32_t lost_buffers = 0;
    bool buffer_loss_flag = false;
    uint64_t records_read = 0;
};

struct EtlExtendedStack {
    uint64_t match_id = 0;
    uint8_t pointer_size = 0;
    std::vector<uint64_t> addresses;
};

struct EtlExtension {
    uint16_t type = 0;
    std::string_view data;                  // Borrowed raw item data, valid only during the callback.
    std::optional<EtlExtendedStack> stack;  // Present only for STACK_TRACE32/64.
};

struct EtlRecord {
    uint64_t qpc = 0;
    std::optional<uint32_t> pid;
    std::optional<uint32_t> tid;
    uint16_t group = UINT16_MAX;  // kernel group, or UINT16_MAX for GUID providers
    std::string provider;
    uint16_t version = 0;
    uint16_t event_id = 0;
    uint8_t opcode = 0;
    uint8_t pointer_size = 0;
    bool extended_data = false;
    std::vector<EtlExtension> extensions;  // Recorded order, including unknown/duplicate types.
    std::string_view payload;              // UserData after extensions; valid only during the callback.
};

// Callback order is physical buffer order, not timestamp order. False cancels.
using EtlRecordCallback = std::function<bool(const EtlFileInfo&, const EtlRecord&)>;
using ImportProgress = std::function<bool(const char* phase, float progress)>;
bool read_etl(std::string_view bytes, EtlFileInfo& info, const EtlRecordCallback& consume, std::string& error,
              const ImportProgress& progress = {});
