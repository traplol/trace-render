# Managed heap snapshot import

TraceRender opens `MemoryProfiler.GCDump` resources referenced by a version 1
`MemoryProfiler.Manifest` inside a desktop `.diagsession`. It records type names,
object counts and bytes. The Memory panel selects snapshots and displays producer
sampling weights and incomplete-data notes. Sessions containing native allocation
history also keep the native outstanding-memory view. Import warnings are under
the collapsed `Import notes` tree.

Snapshots do not establish allocation functions, birth times, per-object survival
between snapshots, or exact free times. This reader creates no allocation
lifetimes or GC survival observations. An unsupported snapshot produces an import
note while supported views remain usable.

## Supported binary layout

The reader accepts the verified `!FastSerialization.1` layout with four-byte
stream labels, `GCHeapDump` version 10/minimum-reader 8, and
`Graphs.MemoryGraph` version 1/minimum-reader 0. It validates type and node tables,
the compressed node blob, child references, addresses, graph totals and trailing
reference bounds. It accepts the observed null/empty JS metadata and version 0
`DotNetHeapInfo`/`GCHeapDumpSegment` metadata. Tagged interop regions are skipped
using their checked forward reference. Deferred graph types, large-graph variants,
different untagged metadata layouts and unknown versions are unsupported.

The format definitions are Microsoft's MIT-licensed
[GCHeapDump](https://github.com/microsoft/perfview/blob/3d89ee8570a943290c4b517663f87493cfbaa35b/src/HeapDump/GCHeapDump.cs),
[MemoryGraph](https://github.com/microsoft/perfview/blob/3d89ee8570a943290c4b517663f87493cfbaa35b/src/MemoryGraph/MemoryGraph.cs),
[Graph](https://github.com/microsoft/perfview/blob/3d89ee8570a943290c4b517663f87493cfbaa35b/src/MemoryGraph/graph.cs), and
[FastSerialization](https://github.com/microsoft/perfview/blob/3d89ee8570a943290c4b517663f87493cfbaa35b/src/FastSerialization/FastSerialization.cs).
The C++ reader implements that bounded layout without a .NET runtime dependency.

Graph nodes with address zero, size zero and bracketed grouping type names are
excluded from object counts. Other addressless or undefined nodes are retained
with an incomplete marker. Free-region and COM records stay graph records; they
never become inferred allocations. Stored sampling multipliers remain separate
from raw counts and bytes. A missing completeness flag does not prove a complete
process heap.

## Authentic fixture evidence

`memory-maui-nine-snapshots.diagsession` is the public capture attached to
[dotnet/maui issue 23680](https://github.com/dotnet/maui/issues/23680#issuecomment-3804661107),
downloaded from the author's
[shared file](https://drive.google.com/file/d/17PGjt0rCuZr_X1nG5lkIBy_BwR3kedHZ/view).
It is 38,880,293 bytes, SHA-256
`4c25dc860848235a9f047b47f75e1c6645c6e666e9602019335d9a6adc3a35c1`,
and records VS 18.0.36711.2 and PID 31736. It remains external to git.

| Snapshot | Recorded objects | Recorded bytes | Excluded grouping nodes |
| --- | ---: | ---: | ---: |
| 1 | 214168 | 14042936 | 10733 |
| 2 | 309675 | 19946112 | 18531 |
| 3 | 454135 | 28836104 | 27238 |
| 4 | 590429 | 36852568 | 34218 |
| 5 | 737780 | 47159576 | 44710 |
| 6 | 873091 | 54796800 | 51423 |
| 7 | 994330 | 61639056 | 59925 |
| 8 | 1114689 | 68209536 | 69814 |
| 9 | 1118691 | 68381464 | 70475 |

All excluded nodes in these nine graphs have zero size and a type among `[1]`,
`[2]`, `[64]`, `[2147483649]`, and `[ROOT]`. Average count and byte multipliers are
one. The embedded dump process/time fields are zero, so the importer uses the
manifest's process ID and `SnapshotTime` in nanoseconds. This unit is checked
against the ETL start FILETIME and each heap's .NET `StartTime`: every snapshot
falls 0.215 to 0.421 seconds after collection starts, within the 182-second trace.
Snapshot 1 is 8.9763981 seconds; snapshot 9 is 157.3224845 seconds.

The ETL contains 1,497 CLR AllocationTick v4 events and 38 GC starts/stops, with
no sampled allocation events 20/32, surviving/moved ranges 21/22 or generation
ranges 23. It does not validate allocating-function or object-survival analysis.

The two official
[PerfView graph fixtures](https://github.com/microsoft/perfview/tree/3d89ee8570a943290c4b517663f87493cfbaa35b/src/PerfView.Tests/GraphSerialization/inputs)
have independently generated XML baselines. `test1` is a JavaScript graph and
validates serialization only. `test2` is a .NET graph that includes free regions
and COM graph records. The tests compare every type's recorded count and bytes
against the XML, including the exclusion of synthetic groups. Their pinned
revision and SHA-256 checksums are in `scripts/gcdump_fixtures.py`.

```bash
python3 scripts/gcdump_fixtures.py /tmp/trace-gcdump-fixtures
TRACE_GCDUMP_FIXTURES=/tmp/trace-gcdump-fixtures \
TRACE_MANAGED_MEMORY_FIXTURE=/path/to/memory-maui-nine-snapshots.diagsession \
./build/trace_render_tests --gtest_filter='ManagedSnapshot.*:ManagedMemoryFixture.*'
```

The production MAUI test checks all nine totals and timestamps, absence of
invented allocation/survival histories, and byte-stable profile save/reopen.
Malformed-data, partial-resource and snapshot-UI checks run without external
fixtures. The separate
[synthetic Windows allocation workload](../tests/fixtures/managed_memory/README.md)
prepares a capture for allocation and GC-survival validation. Snapshot support
alone does not complete TRACE-11's allocation acceptance criteria.
