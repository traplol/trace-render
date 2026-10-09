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
TRACE_RENDER_DIAGSESSION_CORPUS=test_data/diagsession \
./scripts/run_tests.sh
```

The OBS CPU check expects 25,232 samples, 3,944 correlated stack keys and 4,475
stack parts. It checks the recorded Qt module size and exact PDB identity. The
compound capture exercises real DiagnosticsHub compression. The native memory
test independently decodes six producer `.heapstate` snapshots and compares
their 22,798 live allocations, sizes, birth times, and stacks with ETL replay,
then reopens the saved TraceRender profile. Missing optional fixture paths cause
these integration tests to skip; malformed framing, reuse, loss, cross-resource
correlation, recursion, and equal-time observations run in the regular suite.
