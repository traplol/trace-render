#include "native_symbol_resolver.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#ifdef TRACE_RENDER_HAS_NATIVE_PDB
#include <llvm/DebugInfo/PDB/IPDBSession.h>
#include <llvm/DebugInfo/PDB/IPDBLineNumber.h>
#include <llvm/DebugInfo/PDB/IPDBSourceFile.h>
#include <llvm/DebugInfo/PDB/IPDBRawSymbol.h>
#include <llvm/DebugInfo/PDB/PDBSymbolExe.h>
#include <llvm/DebugInfo/PDB/PDB.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace {
std::string hex(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::string basename(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return std::filesystem::path(path).filename().string();
}

// Canonical GUID/age, with no filename or release-label fallback.
std::string build_id(const std::string& text) {
    auto slash = text.find('/');
    if (slash == std::string::npos) return {};
    std::string guid;
    for (char c : text.substr(0, slash)) {
        if (c == '{' || c == '}' || c == '-') continue;
        if (!std::isxdigit((unsigned char)c)) return {};
        guid += (char)std::toupper((unsigned char)c);
    }
    uint32_t age = 0;
    const char* begin = text.data() + slash + 1;
    auto parsed = std::from_chars(begin, text.data() + text.size(), age);
    if (guid.size() != 32 || begin == text.data() + text.size() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        return {};
    return guid + "/" + std::to_string(age);
}
}  // namespace

struct NativeSymbolResolver::Impl {
    explicit Impl(std::vector<std::string> candidates) : paths(std::move(candidates)) {}
    std::vector<std::string> paths;
#ifdef TRACE_RENDER_HAS_NATIVE_PDB
    struct PdbFile {
        std::unique_ptr<llvm::pdb::IPDBSession> session;
        std::string identity;
        std::string error;
        std::unordered_map<uint32_t, NativeSymbol> symbols;
    };
    std::unordered_map<std::string, PdbFile> files;

    PdbFile& open(const std::string& path) {
        auto [it, inserted] = files.try_emplace(path);
        auto& file = it->second;
        if (!inserted) return file;
        // Extracted PDB resources can have arbitrary filenames. Probe their contents first.
        auto error = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native, path, file.session);
        if (error) {
            file.error = llvm::toString(std::move(error));
            error = llvm::pdb::loadDataForEXE(llvm::pdb::PDB_ReaderType::Native, path, file.session);
            if (error) {
                file.error += "; " + llvm::toString(std::move(error));
                file.session.reset();
                return file;
            }
            file.error.clear();
        }
        auto global = file.session->getGlobalScope();
        if (!global) {
            file.error = "PDB identity is unavailable";
            file.session.reset();
            return file;
        }
        std::string guid;
        llvm::raw_string_ostream out(guid);
        out << global->getGuid();
        out.flush();
        file.identity = build_id(guid + "/" + std::to_string(global->getAge()));
        return file;
    }

    const NativeSymbol& lookup(PdbFile& file, uint32_t rva) {
        auto [it, inserted] = file.symbols.try_emplace(rva);
        auto& result = it->second;
        if (!inserted) return result;
        auto symbol = file.session->findSymbolByRVA(rva, llvm::pdb::PDB_SymType::Function);
        if (symbol) {
            const auto& raw = symbol->getRawSymbol();
            auto start = raw.getRelativeVirtualAddress();
            if (rva < start || rva - start >= raw.getLength()) symbol.reset();
        }
        // Public records do not establish a function's extent. Resolve exact starts only.
        if (!symbol) {
            symbol = file.session->findSymbolByRVA(rva, llvm::pdb::PDB_SymType::PublicSymbol);
            if (symbol && symbol->getRawSymbol().getRelativeVirtualAddress() != rva) symbol.reset();
        }
        if (!symbol || symbol->getRawSymbol().getName().empty()) {
            result.diagnostic = "No function record covers RVA " + hex(rva);
            return result;
        }
        result.resolved = true;
        result.name = symbol->getRawSymbol().getName();
        result.symbol_id = "/fn/" + hex(symbol->getRawSymbol().getRelativeVirtualAddress());
        auto lines = file.session->findLineNumbersByRVA(rva, 1);
        if (lines) {
            if (auto line = lines->getNext()) {
                if (auto source = file.session->getSourceFileById(line->getSourceFileId())) {
                    result.source_file = source->getFileName();
                    result.source_line = line->getLineNumber();
                }
            }
        }
        return result;
    }
#endif

    std::vector<std::string> candidates(const ProfileModule& module) const {
        std::vector<std::string> result;
        auto pdb_name = basename(module.pdb_path);
        auto binary_name = basename(module.path.empty() ? module.name : module.path);
        if (pdb_name.empty()) {
            auto path = std::filesystem::path(binary_name);
            path.replace_extension(".pdb");
            pdb_name = path.string();
        }
        for (const auto& path : paths) {
            std::error_code error;
            if (std::filesystem::is_directory(path, error)) {
                if (!pdb_name.empty()) result.push_back((std::filesystem::path(path) / pdb_name).string());
                if (!binary_name.empty()) result.push_back((std::filesystem::path(path) / binary_name).string());
            } else
                result.push_back(path);
        }
        return result;
    }
};

NativeSymbolResolver::NativeSymbolResolver(std::vector<std::string> paths)
    : impl_(std::make_unique<Impl>(std::move(paths))) {}
NativeSymbolResolver::~NativeSymbolResolver() = default;

bool NativeSymbolResolver::available() {
#ifdef TRACE_RENDER_HAS_NATIVE_PDB
    return true;
#else
    return false;
#endif
}

NativeSymbol NativeSymbolResolver::resolve(const ProfileModule& module, uint64_t address,
                                           std::optional<double> observation_ts) {
    NativeSymbol result;
    result.name = hex(address);
    result.symbol_id = module.id + "/ip/" + hex(address);
    if (address < module.load_address || module.size_bytes == 0 || address - module.load_address >= module.size_bytes) {
        result.diagnostic = "Address is outside the captured module range";
        return result;
    }
    uint64_t rva = address - module.load_address;
    result.name = basename(module.name.empty() ? module.path : module.name) + "+" + hex(rva);
    if (observation_ts && ((module.load_ts && *observation_ts < *module.load_ts) ||
                           (module.unload_ts && *observation_ts >= *module.unload_ts))) {
        result.diagnostic = "Observation is outside the captured module lifetime";
        return result;
    }
    auto expected = build_id(module.build_id);
    if (expected.empty()) {
        result.diagnostic = "Captured PDB GUID/age is missing or invalid";
        return result;
    }
    if (rva > UINT32_MAX) {
        result.diagnostic = "Module RVA exceeds the PDB address range";
        return result;
    }
#ifndef TRACE_RENDER_HAS_NATIVE_PDB
    result.diagnostic = "Native PDB support is unavailable in this build";
#else
    result.diagnostic = "No matching local PDB; expected " + module.build_id;
    bool identity_mismatch = false;
    for (const auto& path : impl_->candidates(module)) {
        auto& pdb = impl_->open(path);
        if (!pdb.session) {
            if (!identity_mismatch) result.diagnostic = "Could not read " + path + ": " + pdb.error;
            continue;
        }
        if (pdb.identity != expected) {
            identity_mismatch = true;
            result.diagnostic =
                "PDB identity mismatch for " + path + "; expected " + module.build_id + ", found " + pdb.identity;
            continue;
        }
        auto symbol = impl_->lookup(pdb, (uint32_t)rva);
        if (!symbol.resolved) {
            result.diagnostic = symbol.diagnostic;
            return result;
        }
        symbol.symbol_id = module.id + symbol.symbol_id;
        return symbol;
    }
#endif
    return result;
}

void NativeSymbolResolver::resolve_profile(TraceModel& model) {
    std::unordered_map<std::string, const ProfileModule*> modules;
    for (const auto& module : model.profile().modules) modules.emplace(module.id, &module);
    for (uint32_t i = 0; i < model.stack_frames().size(); ++i) {
        const auto& frame = model.stack_frames()[i];
        if (!frame.address || frame.symbol_resolved) continue;
        auto module = modules.find(model.get_string(frame.module_id));
        if (module == modules.end()) continue;
        auto result = resolve(*module->second, *frame.address);
        model.set_stack_frame_symbol(i, result.name, result.symbol_id, result.source_file, result.source_line,
                                     result.resolved);
        if (!result.resolved) model.add_symbol_warning(module->second->name + ": " + result.diagnostic);
    }
    model.build_index();
}
