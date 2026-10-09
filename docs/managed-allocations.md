# Managed allocation import

TraceRender opens the checked Visual Studio .NET Object Allocation captures on
Linux and attributes recorded allocations to captured allocating stacks. GC
survival is enabled only for a process with one identified Desktop CLR 4 runtime
and a complete, consistent collection stream. CoreCLR allocations retain their
origins, but GC survival stays unavailable because the checked .NET 8 capture
contradicts the workload's retained-object checks.

Totals describe recorded allocations. The supported resources do not establish
the producer's sampling rate or the contents of the initial heap. Import marks
this coverage as incomplete and records a sampling warning. It does not scale
counts or claim that the totals cover every managed object.

## Supported records and identities

The checked x64 provider is `8bc9e67b-ca34-4b9a-9442-8f75403f357b`.
Its event payload starts after the ETL EventHeader extension items.

| Event | Checked payload |
| --- | --- |
| Allocation, ID 1 version 4 | Little-endian uint64 profiler ID, uint32 size, uint32 type-descriptor byte length, that many descriptor bytes, trailing uint32 field. The descriptor remains opaque; the trailing field is not used to infer survival. |
| Collected objects, ID 2 version 1 | Zero uint32 prefix, uint32 count, exactly that many uint64 profiler IDs. |

Allocation IDs are capture-local profiler identities, not heap addresses.
Replay scopes them to the normalized process lifetime. Reused IDs within a
process invalidate its survival observations instead of joining different
allocations. The opaque type descriptor does not supply an allocating function.

Only one captured x64 inline stack supplies unambiguous ancestry. Missing,
multiple, or wrong-width stack parts leave the origin unresolved while keeping
the allocation. Recorded CLR method ranges resolve managed frame names.
Retaining references and type names never become allocation origins.

Public CLR provider `e13c0d23-ccbc-4e12-931b-d9cc2eee27e4` supplies
RuntimeInformation ID 187 version 0, GCStart ID 1 version 2, and GCEnd ID 2
version 1. The runtime gate requires SKU 1 and VM major 4. A process with no
runtime identity, CoreCLR, conflicting runtime identities, or multiple CLR
instances gets allocation history without GC observations. Private collection
records do not carry a proven CLR-instance discriminator. The public layouts,
SKU values, and GC mechanism flags are documented in Microsoft's
[TraceEvent CLR parser](https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/ClrTraceEventParser.cs).

Replay orders records by QPC, associates collection batches with matching
GC start/end numbers and CLR instances, and records absence at the completed GC
end. A complete blocking generation-2 GC also supplies positive observations for
remaining recorded identities. A minor collection can report an object's
absence; it does not prove survival of every other generation. Background or
ambiguous collections, malformed relevant records, allocation during a blocking
GC, unknown collected IDs, repeated collected IDs, incomplete GC pairs,
nonconsecutive GC numbers after the first observed collection, and
reported event loss disable survival for the affected history. Valid allocation
records and origins remain available.

No collection checkpoint becomes an exact `freed_ts`, a physical address, or a
claim of liveness at capture end. Queries can prove survival through positive
checkpoints and absence at or after negative checkpoints. Unobserved intervals
and the tail remain uncertain. The birth-range filter selects `[start, end)` by
observed allocation time. `.trprofile` preserves identities, origins,
observations, and quality; reopening rebuilds the same queries.

## Authentic fixture provenance

The original workload is in `tests/fixtures/managed_memory/Program.cs`. It roots
2,048 `RetainedPayload` objects in a static list, releases 4,096
`ReleasedPayload` objects, and holds then releases a 128 KiB byte array. Each
post-GC checkpoint reads the retained array lengths and checks weak references.
Both runtimes report 2,048 retained payloads and 8,388,608 retained content bytes;
the large-object weak reference is true while held and false after release.

[Workflow run 37868691292](https://github.com/traplol/trace-render/actions/runs/37868691292)
used generator commit
[`9d47a3b8f1c89ef822dca5fce57bba34ea138dc6`](https://github.com/traplol/trace-render/commit/9d47a3b8f1c89ef822dca5fce57bba34ea138dc6)
on `windows-2022`, Visual Studio Diagnostics 17.14.37606.2, and the installed
`DotNetObjectAllocBase.json`. Its config SHA-256 is
`11619460834073ee222540318afab1bf0e995a7dfc442c519d458cac91e1daa5`.
The captures were produced on 2026-10-09 UTC. Artifact names are
`managed-memory-capture-net48` and `managed-memory-capture-net8.0`.

| Runtime | Capture bytes | SHA-256 of `managed-allocation-survival.diagsession` |
| --- | ---: | --- |
| .NET Framework 4.8, workload CLR version 4.0.30319.42000 | 3,658,727 | `58d304a10ed54716129bb0a7f77f2a46109547f9c92be33802ba739a8892d7f0` |
| .NET 8.0.31 | 3,630,912 | `255b572f1f24ac33069fc3061bc1da5aa07b2ef69561ccb5a553c14bf076a11a` |

The artifacts contain original captures, binaries/PDBs, collector configuration,
logs, provenance and hashes, workload checkpoints, and native Windows `tracerpt`
XML/schema exports. Artifact retention is seven days, so these links are not
permanent fixture storage. Before expiry, download both artifacts and verify the
capture hashes above. The binary captures are not committed to the repository.

Regenerate on Windows with Visual Studio Diagnostic Tools and the required .NET
build tools, using a fresh output directory for each run:

```powershell
./scripts/capture_managed_memory.ps1 -Framework net48 -OutputDirectory C:/temp/managed-net48
./scripts/capture_managed_memory.ps1 -Framework net8.0 -OutputDirectory C:/temp/managed-net8
```

The `Managed memory fixture` workflow runs the same matrix. Regeneration preserves
the workload invariants, but process IDs, clocks, hashes, and runtime housekeeping
allocations change. The pinned tests below intentionally require the original
files. A replacement fixture needs new independently checked expectations; do not
update its numbers from TraceRender's own output.

## Independent evidence and limits

The oracle decoded `tracerpt`'s private-provider `BinaryEventData` and native CLR
method names, then compared every allocation payload and stack with physical ETL
records read separately on Linux. Embedded metadata TypeDefs distinguished the
two workload classes. All 20,133 Desktop and 23,652 Core allocation payloads and
stacks matched; all nine Desktop and thirteen Core collection batches matched.
Every mentioned ID had an earlier allocation, with no duplicate collection IDs.
Both ETLs report zero lost events and buffers. No proprietary implementation was
decompiled, disassembled, or copied.

Desktop RuntimeInformation reports PID 4544, CLR instance 7, SKU 1, BCL 4.0.0.0,
and VM 4.0.30319.0. Core reports PID 7888, instance 9, SKU 2, BCL 8.0.31.0, and VM
8.0.3126.42015. These are recorded event versions, distinct from the workload's
`Environment.Version` strings.

Desktop collection lists omit all 2,048 retained class IDs and 2,048 retained
array IDs, while collecting all 4,096 released class IDs and 4,096 released array
IDs at GC 5. The retained identities survive generation-0 and generation-1
collections, then compacting full collections. GCGlobalHeapHistory records
compaction in its mechanism flags at GC 5, 7, and 9. The same large-object ID
survives GC 7 and is reported collected at GC 9 after its root is removed.

Core's GC 4 collection list includes every retained class and retained array ID,
despite the subsequent successful reads of all 8 MiB of content. The native
Windows export contains this contradiction too. Its internal producer/runtime
cause is unresolved. This capture proves allocation-origin decoding and the need
to withhold CoreCLR survival; it does not establish broad CoreCLR GC support.

The fixed clock is 10,000,000 QPC ticks per second. Desktop's ETL origin is
`12598809889`; Core's is `10965367735`. TraceRender time in microseconds is
`(event_qpc - origin_qpc) / 10`.

| Desktop completed GC | End QPC | Recorded live objects | Recorded live bytes |
| --- | ---: | ---: | ---: |
| 5 | 12633453567 | 5,812 | 8,683,904 |
| 7 | 12633652060 | 5,836 | 8,816,564 |
| 9 | 12633732427 | 5,836 | 8,685,532 |

Desktop records 20,133 allocations totaling 17,881,227 bytes. `AllocateRetained`
has 4,106 inclusive allocations totaling 8,519,888 bytes; 4,097 and 8,503,320 bytes
survive each listed checkpoint. `AllocateReleased` has 8,206 and 8,651,072 bytes;
its two remaining incidental allocations total 48 bytes. The allocation interval
`[12632974194, 12633413016)` QPC contains 12,312 allocations and 17,170,960 bytes.
Its surviving cohort is 4,099 objects and 8,503,368 bytes at all three checkpoints.
Full GC 6, 8, and 10 have no collection batches but still give valid positive
observations.

Core records 23,652 allocations totaling 18,483,185 bytes. The corresponding
retained-function totals are 4,106 and 8,519,888 bytes; released-function totals
are 8,206 and 8,651,064 bytes. At QPC `11016400054`, all 22,849 allocations already
recorded, totaling 18,419,683 bytes, remain uncertain in the absence of trusted
survival observations.

Run the production importer and save/reopen proofs after building:

```sh
TRACE_MANAGED_DESKTOP_FIXTURE=/path/net48/managed-allocation-survival.diagsession \
TRACE_MANAGED_CORE_FIXTURE=/path/net8/managed-allocation-survival.diagsession \
  ./build/trace_render_tests --gtest_filter='ManagedAllocation*.*'
```

These tests pin capture sizes and raw clock anchors, then check independent
global, allocating-function, constructor-array, interval, and survival results.
Hashes above identify the files without adding a cryptography dependency to the
test target. Missing environment variables produce explicit skips. Ordinary wire
and replay unit tests still run. The older `TRACE_MANAGED_ALLOCATION_FIXTURE`
variable belongs to the separate ETL-framing proof for the original 23,188-event
capture; it is not interchangeable with either runtime fixture here.
