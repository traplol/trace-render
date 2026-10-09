# Portable diagsession import

The desktop parser recognizes ZIP and OLE/CFB signatures, reads DiagnosticsHub
metadata, and imports resources whose recorded type is
`DiagnosticsHub.Resource.EtlFile`. Filenames and archive order do not select the
trace. All referenced ETLs share one collector so a stack or module in one
resource can describe a sample in another. Their QPC frequency and Windows boot
must agree. The browser build omits this importer and its dependencies.

Containers stay in memory. ZIP uses pinned miniz 3.1.2; metadata XML uses
tinyxml2 11.0.0. CFB versions 3 and 4 support FAT, miniFAT, DIFAT, and Unicode
property dictionaries that map logical resource IDs to storage names. Resource
data is read lazily, so CPU import does not expand minidumps. DiagnosticsHub
compression wrappers contain raw or XPRESS-Huffman chunks; both stored and
expanded CRCs are checked. Each expanded resource has a 2 GiB limit and an
explicit error when it exceeds that limit. Nothing is extracted to disk.

The ETL reader checks buffer and record boundaries in release builds. It handles
System, Compact, PerfInfo, classic Full/Instance, and EventHeader framing, plain
XPRESS and XPRESS-Huffman compression, and QPC timestamps. Unknown header types,
unsupported compression, invalid sizes, and truncation fail with an error.
EventHeader extended payloads are reported as a decoding limitation. Loss
counters, buffer loss flags, skipped native heap records, and unmatched stacks
remain visible in profile quality metadata.

CPU observations remain point samples. Each record contributes one sample, and
only an explicitly recorded Timer interval supplies estimated CPU time. The raw
SampleProf Count and flags are retained; Count is not used as a multiplier.
Microsoft's public [SampledProfile documentation](https://learn.microsoft.com/en-us/windows/win32/etw/sampledprofile)
describes it as unused. Sample arguments retain raw QPC, frequency, sampled IP,
source resource, and every correlated StackWalk part. Thread generation and
observed start/end QPC are saved on observations. Process and module generations
have explicit identities; rundown does not invent creation or destruction times.
Unknown frames keep their addresses and stable identities.

CLR method load, unload, and rundown events resolve managed CPU frames in the
same pass. Runtime method events 137–144 and rundown events 141–144 support
versions 0, 1, and 2. The layouts come from the official
[runtime manifest](https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/ClrEtwAll.man)
and [PerfView parser](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/ClrTraceEventParser.cs).
Verbose events supply captured namespace, method name, and signature; no user
binaries are required. Nonverbose ranges retain managed identities and an
explicit unresolved name. Standalone metadata caches and JIT-start events without
code ranges do not currently add names.

Method identities include the process generation, CLR instance, module, method,
ReJIT ID, code extent, and code generation. An actual load/unload bounds its
range. Rundown is an enumeration of existing code, so its timestamp is not used
as a compilation time. It may describe earlier samples within the same process
generation. Recorded reuse constrains that inference. Overlapping shared generic
code, conflicting names, and unrecorded transitions remain unresolved rather than
choosing an arbitrary method. Unsupported or malformed transitions stop older
method attribution until a later known load. Quality warnings explain these
limits and the possibility of transitions missing before recording began.

Mixed stacks keep every recorded address and recursive frame in order. A known
managed range is marked `Managed`; a mapped native image is `Native`; addresses
without either attribution are `Unresolved`. Resolved names and generation
identities survive profile save/reopen. Managed PDB/IL-to-native source mapping is
not implemented yet and is reported explicitly. The compound fixture contains
portable PDBs for other libraries, but its recorded `SimpleFunction.pdb` identity
is absent; those unrelated symbols must not supply its source locations.

Native x64 heap allocation/free/reallocation version 2 records use the same pass,
with recorded allocation stacks. Heap create version 3 is also decoded. The
native lifetime engine distinguishes missing initial contents from an internal
recording gap, and preserves the original allocation in the Windows
Alloc/Free/Realloc-summary sequence. Other heap layouts produce a quality
limitation. Managed heaps, minidumps, counters, and other non-ETL resources are not
decoded by this importer yet.

The file layout references are the Windows WDK definitions and the
[MIT Snail reader at 71b01259](https://github.com/albertziegenhagel/snail-server/tree/71b01259fff9e1ef347a77a7c77d90e35699e1b4/snail/etl).
The traversal and checked reads are implemented here without its server or
analysis dependencies. Decompression follows Microsoft's
[MS-XCA specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/).
The external AGPL inspection tools mentioned in the corpus proof are not linked
or copied into the application.

## Verification

The checked fixtures and acquisition instructions are in
[diagsession-corpus.md](diagsession-corpus.md). The older compound capture comes
from [PerfView issue 1819](https://github.com/microsoft/perfview/issues/1819).
Extract `Report20230224-2305.diagsession` from its
[attached ZIP](https://github.com/microsoft/perfview/files/10843122/Report20230224-2305.zip)
as `test_data/diagsession/cpu-perfview-compound.diagsession`. Its SHA-256 is
`77b2a86e0dde209ce397c0276af4371cf2aa59acd0b17bbecfb113d745b6f70f`.
After fetching the captures, run:

```sh
cmake -B build
TRACE_NATIVE_CPU_FIXTURE=test_data/diagsession/native-cpu-obs.diagsession \
TRACE_NATIVE_MEMORY_FIXTURE=test_data/diagsession/native-memory-cscn.diagsession \
TRACE_COMPOUND_CPU_FIXTURE=test_data/diagsession/cpu-perfview-compound.diagsession \
TRACE_MANAGED_CPU_FIXTURE=test_data/diagsession/cpu-azure-durabletask.diagsession \
TRACE_RENDER_DIAGSESSION_CORPUS=test_data/diagsession \
./scripts/run_tests.sh
```

The OBS CPU check expects 25,232 samples, 3,944 correlated stack keys and 4,475
stack parts. It checks the recorded Qt module size and exact PDB identity. The
compound capture exercises real DiagnosticsHub compression. The native memory
test independently decodes six producer `.heapstate` snapshots and compares
their 22,798 live allocations, sizes, birth times, and stacks with ETL replay,
then reopens the saved TraceRender profile. The Azure managed capture contains
31,664 samples and 23,689 correlated stacks. Its checked sample at QPC
1127633188921 belongs to PID 6592, TID 4688, and retains a 129-frame mixed stack
with repeated DurableTask async methods. The compound capture contains 69,833
samples; QPC 332187593497, PID 29796, TID 3860 includes `Program.Main`, the
`NeuralNetwork` constructor, and `InitBiases` in recorded order. Method names,
addresses, and process names were checked against raw CLR/StackWalk payloads.
Missing optional fixture paths cause
these integration tests to skip; malformed framing, reuse, loss, cross-resource
correlation, recursion, and equal-time observations run in the regular suite.

## Desktop workflow

On Linux, open a `.diagsession` using File > Open, drag it into the window, or pass
its path as the application argument. These paths use the same local decoder.
Import runs in the background; the overlay shows the current phase and its
progress. Cancel or Escape discards the unfinished result. An import error never
publishes a partial model as a completed capture.

Settings > Symbols accepts one local PDB file, matching binary, or directory per
line. Reopen the capture to use new paths. Native embedded MSF/PDB resources are
read in memory. Every native match still requires the captured PDB GUID and age.
Missing symbols leave module offsets or raw addresses usable. The Diagnostics
panel lists available capabilities and import notes. Native PDB resolution needs
the optional installed LLVM dependency described in [native-symbols.md](native-symbols.md).
The ZIP/XML decoder dependencies are built into the desktop target; there is no
Windows helper or manual conversion step.

File > Save Profile writes a versioned `.trprofile`. Reopening it preserves CPU
samples, allocation lifetimes, coverage markers, and already-resolved names and
source locations without the original capture or symbol files. Source code itself
is not copied into the profile. Saving reports write errors in the status bar.
Chrome JSON opening and the existing CSV/TSV exports remain available.

The original offline embedded-PDB test and `FileLoader` tests exercise cancellation,
retry, symbol resolution during loading, and saved names with unavailable symbol
paths. The authentic fixture environment variables above also exercise the actual
desktop loader on both native captures, including outstanding-memory queries after
saving and reopening.
