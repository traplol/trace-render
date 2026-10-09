# Native diagsession corpus

TRACE-3 verifies local Linux decoding of authentic native CPU and memory captures.
The memory proof compares replayed ETL allocation lifetimes with six independently
serialized Visual Studio heap snapshots, including their allocation stacks.
These scripts are fixture tools. They do not implement application import.

## Reproduce

Use Python 3.11 or newer. From the repository root, download the two pinned files:

```sh
python3 scripts/diagsession_fixtures.py fetch
```

The files go into Git-ignored `test_data/diagsession/`. The downloader checks their
SHA-256 hashes and reuses an existing matching file. It does not execute captured
binaries. `tests/fixtures/diagsession/corpus.json` records exact source URLs,
producer versions, resource inventories, checksums and expected event counts.
The capture authors have not supplied a redistribution license, so the repository
contains download instructions rather than their binaries or extracted dumps.

Install the standalone inspection tool in a temporary environment and verify:

```sh
python3 -m venv /tmp/trace-fixtures-venv
/tmp/trace-fixtures-venv/bin/pip install dissect.etl==3.14
/tmp/trace-fixtures-venv/bin/python scripts/diagsession_fixtures.py verify
python3 -m unittest discover -s tests -p 'test_diagsession_fixtures.py'
```

Verification prints one PASS line per capture and writes
`test_data/diagsession/verification.json`. It checks ZIP CRCs, inventory, producer
version, event counts, timestamps, stack correlation, and the native snapshot
oracle. Run normal Python without `-O`, since the proof uses assertions.

Run the production native memory importer regression after building the C++ tests:

```sh
TRACE_NATIVE_MEMORY_FIXTURE="$PWD/test_data/diagsession/native-memory-cscn.diagsession" \
  ./build/trace_render_tests --gtest_filter='NativeMemoryFixture.*'
```

The opt-in test opens the original capture with the application importer and reads
the six saved heapstate resources independently. It compares every outstanding
allocation's address, size, birth time and stack, stack-group and cohort totals,
and the same outstanding queries after a `.trprofile` save/reopen. Without the
environment variable, the large external-fixture test reports skipped; the
synthetic lifetime/query regressions run in the normal suite.

[Dissect ETL](https://github.com/fox-it/dissect.etl) is AGPL-3.0 tooling used only
for independent inspection here. It is not an application dependency; no Dissect
source is copied into TraceRender. Its optional manifest decoder misreads pointer
fields in this corpus, so the verifier reads explicit x64 payloads from Microsoft's
[heap event schema](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/kerneltraceeventparser.mof).

## Captures

| Capture | Producer and mode | Architecture | SHA-256 |
| --- | --- | --- | --- |
| `native-cpu-obs.diagsession`, 2,981,218 bytes | Visual Studio 17.14.36607.1, CPU sampling, OBS PID 1712 | x64 | `9bc636cacad326af33798dd9bff6a0aec6a15dcd36b22068eada43deb3151125` |
| `native-memory-cscn.diagsession`, 49,023,829 bytes | Visual Studio 17.8.34329.2, native Memory Usage, Server.exe PID 25260 | x64 | `9d23e331bd78f2e1772981662d10aa2f92810b91539504f199687748f58a062d` |

The CPU capture comes from the author's
[OBS issue attachment](https://github.com/obsproject/obs-studio/issues/12758#issuecomment-3475952776).
It contains a 16,187,392-byte ETL, counters and metadata. Linux inspection finds
142,376 records, 25,232 CPU samples and 4,475 StackWalk records. Every StackWalk
has a matching sample timestamp and thread ID; 2,007 belong to OBS. Preserve
multiple stack parts under the same correlation key. The sampled instruction
pointer is separate data and does not always equal the first unwound frame.

The memory capture is the public
[CSCN native telemetry server crash test](https://github.com/CecilFoubert/Project-6---Group-4---CSCN73060-SEC-1/blob/110abc0288a609a343392836d619f38061c293ef/PROJ2CODE/Crash_Test_InClass.diagsession)
at commit `110abc0288a609a343392836d619f38061c293ef`. The raw Git URL returns an
LFS pointer; the pinned `media.githubusercontent.com` URL in the corpus manifest
returns the actual capture. The container has a 129,564,672-byte ETL, six
`.heapstate` files, six native minidumps, counters, and a MemoryProfiler manifest.
That manifest explicitly sets `IsNativeEnabled=true`, `IsManagedEnabled=false`,
and identifies all six snapshots as `PROFILER_NATIVE`.

Linux ETL decoding finds 1,714,268 records, including 311,635 allocations, 307,133
frees and one realloc summary from provider
`222962ab-6180-4b88-a825-346b75f2a24a`. Every allocation and the realloc summary
has an allocation StackWalk matched by raw QPC, PID and TID. Address reuse occurs
for 1,324 addresses, with up to 16,926 recorded generations of one address.

Both ETLs use an 8-byte pointer size, a 10,000,000 Hz QPC clock and report zero
lost events or buffers. These are producer loss counters, not a promise that every
kind of allocation was recorded. The scope demonstrated here is Windows heap
allocation, not suballocations within custom pools, GPU memory or virtual memory.

## Independent memory oracle

The verifier reads the producer's heapstate tables independently of ETL replay.
This observed VS 17.8 x64 layout is specific to the pinned fixture, not a general
heapstate format contract:

- The first four uint32 values are `0x407208f6, 2, 8, 0`.
- QPC values at offsets 16, 24 and 32 mark session start, snapshot start and
  snapshot end. Offset 52 contains four tables; table data begins at `0x240`.
- Each table has a 16-byte identifier followed by uint32 version, record size and
  count. All versions are 1. Record sizes are 24, 32, 32 and 8 bytes.
- Table 0 stores total bytes, live bytes, allocation count and live count.
  Table 1 stores allocation QPC, size, address and stack ID as four uint64 values.
  Table 2 stores frame-array start, depth, total/live bytes and total/live counts.
  Table 3 stores uint64 frame addresses.

All tables exhaust each file exactly. Allocation totals and per-stack live counts
and bytes agree with the producer's summary tables. ETL replay at each
`snapshot_end_qpc` then matches every live address, size and allocation timestamp:

| Snapshot | End QPC | Outstanding allocations | Outstanding bytes |
| --- | ---: | ---: | ---: |
| 0 | 883681963340 | 276 | 86,779 |
| 1 | 884799041469 | 4,504 | 1,069,378 |
| 2 | 887346737139 | 4,512 | 1,072,650 |
| 3 | 888414448792 | 4,502 | 1,068,330 |
| 4 | 889601553015 | 4,502 | 1,068,330 |
| 5 | 889970253814 | 4,502 | 1,068,330 |

The verifier checks 22,798 live allocation entries across those snapshots. Every
saved allocation stack equals its ETL stack with the final root frame omitted.
Keep the full ETL stack in imported data. The snapshots omit terminal addresses
`0x7ffabd102651` or `0x7ffabd124b0e` consistently in this capture.

Two details matter for the production lifetime reader:

- The one realloc emits `Alloc(new)` at QPC 883562417062, `Free(old)` at
  883562417096 and a ReAlloc summary at 883562417100. The new block is 564 bytes;
  the old block was 308 bytes. Counting the summary again changes its allocation
  timestamp and stack. The proof validates the summary while retaining the
  original allocation. Standalone, in-place and failed reallocations require
  separate coverage before broader support is claimed.
- Snapshot-start cutoffs include two to five transient profiler allocations.
  Snapshot-end cutoffs reproduce the producer's saved state exactly. Do not
  substitute metadata timestamps for observed record clocks.

The end of the recording also has 4,502 outstanding allocations and 1,068,330
bytes. This proves surviving allocation attribution; it does not establish
whether the application intended to retain them. The four small Python tests
are synthetic lifecycle cases, separate from authentic-capture verification.
They cover intentional retention, release, address reuse, process/heap identity,
the realloc-summary rule and rejection of incomplete fixture data.

## Symbols

No matching Server.exe PDB is published with the memory capture. Preserve its
allocation stacks as addresses and module offsets; function-name completeness
cannot be established from the source alone.

The OBS author links Qt symbols from
[obs-deps 2025-08-23](https://github.com/obsproject/obs-deps/releases/tag/2025-08-23).
The `windows-deps-qt6-2025-08-23-x64-PDBs.zip` archive SHA-256 is
`aae88a17e0211cb37db6a8602f2e20d69255be1f9700c699008ca5adbce1dde2`.
LLVM 18's Linux PDB reader verified GUID and age against the trace:

| PDB | Recorded image base | GUID | Age |
| --- | --- | --- | ---: |
| Qt6Core.pdb | `0x7ffbd0580000` | `281508b9-c415-4b5c-9ac2-fe4844313239` | 1 |
| Qt6Gui.pdb | `0x7ffbd0b60000` | `8c9a318b-e6c6-4c62-9145-87c1abb0825a` | 1 |
| Qt6Widgets.pdb | `0x7ffbd1560000` | `1a6ed01c-6ce2-40ae-8959-60a52daebe62` | 1 |

Using only a matching PDB and observed relative address on Linux resolved
Qt6Widgets `0x5495c` to `QWidgetPrivate::drawWidget`, `qwidget.cpp:5658`, and
Qt6Core `0x9e91f` to `QCoreApplication::notifyInternal2`,
`qcoreapplication.cpp:1177`. These manual LLVM checks are additional evidence,
not assertions performed by the corpus verifier. TRACE-6 owns production symbol
resolution and its regression checks.

## Production reader path and limits

The demonstrated path is ZIP resource extraction, portable ETL framing, explicit
event decoding, QPC/PID/TID stack correlation, and allocation replay keyed by
process, heap and address generation. It needs no Windows service at analysis
time. The native heap payload layouts are in Microsoft's MIT-licensed
[PerfView schema](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/kerneltraceeventparser.mof).
Use recorded samples and lifetimes as the data model, then derive display spans.

[Snail's MIT ETL reader](https://github.com/albertziegenhagel/snail-server/tree/71b01259fff9e1ef347a77a7c77d90e35699e1b4/snail/etl)
is a compatible source reference for a C++ implementation. It already decodes
System, PerfInfo, full event and EventHeader records, as seen in this corpus.
Its implementation uses newer C++ facilities, so it is not a drop-in dependency
for TraceRender's C++17 build. Reuse the required reader pieces with their license
notice or implement the documented layouts directly. LLVM's native PDB reader
supplies the demonstrated Linux symbol path.

Both pinned captures have deflated ZIP members and uncompressed ETL buffers.
They do not prove ETL XPRESS, LZNT1 or XPRESS-Huffman support. Snail implements
XPRESS but leaves the latter two unimplemented. The existing OLE compound
captures need a separate container reader. Other header versions, x86, custom
allocators, missing stack events and truncated captures also need explicit
coverage. The import UI must report those limitations rather than silently
presenting partial data as complete.

The search also found a native ECS memory capture in
`jacobs-thomas/entity_component_system`, but its LFS endpoint reports that Git LFS
is disabled. Its pointer alone is not a usable fixture. The public CSCN capture
above supplies the working native memory proof without requiring the user to
record a new profile.

## Managed captures

The managed CPU capture comes from
[Azure/durabletask issue 361](https://github.com/Azure/durabletask/issues/361).
Download the public [diagsession.zip attachment](https://github.com/Azure/durabletask/files/4050870/diagsession.zip),
extract its `error.diagsession` member, and rename it to
`test_data/diagsession/cpu-azure-durabletask.diagsession`. The extracted file is
14,924,022 bytes, SHA-256
`c4221cbb1a67c403f1004d45dc57ec92aee0edaaf4548de6ab01d237effc9981`,
with metadata `_BuildVersion` 15.9.18337.3. Set `TRACE_MANAGED_CPU_FIXTURE` to
that path when running the authentic managed CPU tests.

The nine-snapshot MAUI capture, its original issue/Drive download links, exact
size/hash, supported format and snapshot test command are documented in
[Managed heap snapshot import](managed-snapshots.md). Set
`TRACE_MANAGED_MEMORY_FIXTURE` to the downloaded
`memory-maui-nine-snapshots.diagsession`. These captures are external fixtures;
neither establishes managed allocation-origin and GC-survival coverage.
