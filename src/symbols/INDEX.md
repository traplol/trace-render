# src/symbols/
Optional portable native PDB resolution shared by CPU samples and allocation stacks.

## native_symbol_resolver.h / native_symbol_resolver.cpp — validates captured PDB identities, caches local and embedded LLVM sessions and address lookups, and retains resolved or unresolved frame identities
```
explicit NativeSymbolResolver(std::vector<std::string> paths = {});
~NativeSymbolResolver();
static bool NativeSymbolResolver::available();
NativeSymbol NativeSymbolResolver::resolve(const ProfileModule& module, uint64_t address, std::optional<double> observation_ts = std::nullopt);
bool NativeSymbolResolver::add_embedded_pdb(const std::string& name, std::string_view bytes, std::string& error);
bool NativeSymbolResolver::resolve_profile(TraceModel& model, const std::function<bool(float)>& progress = {});
```
