#pragma once
#include "etl_reader.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

struct ManagedMethod {
    std::string process_id, symbol_id, name;
    uint64_t address = 0, size = 0, method_id = 0, module_id = 0, rejit_id = 0;
    uint32_t flags = 0;
    uint16_t clr_instance = 0;
    std::optional<uint64_t> start_qpc, end_qpc;
};

// CLR code ranges are scoped to process and method generations, including rundown
// whose enumeration timestamp is not a JIT compilation timestamp.
class ManagedMethods {
public:
    bool consume(const EtlRecord& record);
    void build(const std::function<std::string(uint32_t, uint64_t)>& process_at);
    const ManagedMethod* find(const std::string& process_id, uint64_t address, uint64_t qpc) const;
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    enum class Kind { Load, Unload, Rundown };
    struct Record {
        ManagedMethod method;
        uint64_t qpc = 0;
        uint32_t pid = 0;
        Kind kind = Kind::Rundown;
    };
    struct Lifetime {
        ManagedMethod method;
        uint64_t first_seen = 0, last_seen = 0;
        bool load_observed = false, conflicting_name = false;
    };
    struct Range {
        size_t index = 0;
        uint64_t max_end = 0;
    };
    struct Gap {
        uint32_t pid = 0;
        uint64_t qpc = 0;
        bool rundown = false;
    };
    void warn(const std::string& message);
    std::vector<Record> records_;
    std::vector<Lifetime> methods_;
    std::map<std::string, std::vector<Range>> ranges_;
    std::vector<Gap> gaps_;
    std::map<std::string, std::vector<uint64_t>> process_gaps_;
    std::vector<std::string> warnings_;
};
