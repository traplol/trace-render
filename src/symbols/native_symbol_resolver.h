#pragma once
#include "model/trace_model.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct NativeSymbol {
    bool resolved = false;
    std::string name;
    std::string symbol_id;
    std::string source_file;
    uint32_t source_line = 0;
    std::string diagnostic;
};

// One resolver per import or explicit symbol reload. Files, failures, and RVA lookups are cached.
class NativeSymbolResolver {
public:
    explicit NativeSymbolResolver(std::vector<std::string> paths = {});
    ~NativeSymbolResolver();
    static bool available();
    NativeSymbol resolve(const ProfileModule& module, uint64_t address,
                         std::optional<double> observation_ts = std::nullopt);
    void resolve_profile(TraceModel& model);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
