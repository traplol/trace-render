# src/symbols/
Optional portable native PDB resolution shared by CPU samples and allocation stacks.

## native_symbol_resolver.h / native_symbol_resolver.cpp — validates captured PDB identities, caches local LLVM sessions and address lookups, and retains resolved or unresolved frame identities
```
explicit NativeSymbolResolver(std::vector<std::string> paths = {});
~NativeSymbolResolver();
static bool NativeSymbolResolver::available();
NativeSymbol NativeSymbolResolver::resolve(const ProfileModule& module, uint64_t address, std::optional<double> observation_ts = std::nullopt);
void NativeSymbolResolver::resolve_profile(TraceModel& model);
```
