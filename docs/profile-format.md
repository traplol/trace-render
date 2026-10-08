# TraceRender profile version 1

The `.trprofile` file begins with the ten ASCII bytes `TRPROFILE\n`, followed by one UTF-8 JSON object. The object contains `"version": 1`, `"time_unit": "us"`, and `"byte_unit": "bytes"`. `TraceParser::parse()` and `parse_buffer()` recognize the header independently of the filename. The Chrome import time-unit setting does not change profile timestamps.

`serialize_profile()` and `write_profile()` write this format. `read_profile()` validates a complete file and only replaces the destination model after a successful read. `TraceParser` uses that reader in the desktop file-loading path. The source `.diagsession`, binaries, and PDBs are provenance, not dependencies when reopening. Source code itself is not embedded; the Source panel uses saved paths and the existing path-remapping settings.

Writers include all documented tables and fields, including empty arrays, empty strings, and explicit nulls. JSON member order has no meaning. An unsupported version, missing required field, bad reference, invalid unit, or malformed record returns an error naming the affected table. The optional `allocation_history_gaps` quality field defaults to false in older version 1 files. Unknown extra fields have no effect in version 1. Changes that alter existing meanings require another version; readers must reject versions they do not understand.

## Numeric and identity rules

- Timestamps, intervals, and explicit CPU time estimates are finite double-precision microseconds relative to the same capture origin. JSON round-trips the in-memory double value. No duration is inferred from the gap between samples.
- Addresses, byte quantities, object counts, lost-event counts, and event IDs use unsigned decimal **strings** in the file. This preserves every value through `18446744073709551615`, including values above JavaScript's exact integer range. They are `uint64_t` in the model.
- PIDs, TIDs, string/argument indices, and source-line numbers use unsigned 32-bit JSON integers. `sort_index` is signed 32-bit. Source line zero means unavailable.
- `strings` is a deduplicated string array whose first member is `""`. String indices refer to this one table. `4294967295` is the absent-index sentinel only in fields that allow it. `args` is an array of original argument JSON strings, separate from the interned string table.
- Process, heap, module, allocation, type, and stack identities are opaque capture-local strings. A process identity identifies one OS-process lifetime; a heap identity identifies one heap lifetime. A module identity identifies one module load in a process lifetime. An allocation identity identifies one allocation generation. An address or a display name alone is never an identity.

## CPU and existing trace tables

`events` retains input order. Each record has these fields:

| Field | Meaning |
|---|---|
| `name`, `category` | String indices |
| `phase`, `kind` | Existing trace phase and `measured`, `sample`, or `sampled_span` |
| `ts`, `duration` | Timestamp and interval in microseconds |
| `pid`, `tid`, `id` | OS identifiers and the original event ID |
| `process_instance` | Process-lifetime ID string index, or absent sentinel |
| `args` | Argument-array index, or absent sentinel |
| `stack_frame` | Leaf stack ID string index, or absent sentinel |
| `weight`, `weight_unit` | Raw nonnegative weight or null, and its unit string index |
| `estimated_cpu_us` | Supplied CPU time estimate in microseconds or null |

Each `sample` has phase `P`, zero duration, and represents exactly one observation. Repeated observations remain separate. A generic weight is not automatically time. `weight` and `estimated_cpu_us` stay null if unavailable. A `sampled_span` has an estimated interval and unknown original sample count. A `measured` event retains measured duration semantics. Allocated and outstanding bytes never become CPU intervals.

`stack_frames` contains `id`, `name`, `category`, `parent`, `valid`, `module`, `symbol`, `address`, `source_file`, `source_line`, and `symbol_resolved`. The ID, parent, module, symbol, name, category, and source file fields are string indices. Parent, module, and symbol may use the absent sentinel. Address is an optional unsigned decimal string. `symbol` identifies the function independently of its name; `module` identifies its load. Equal display names do not collapse stored frames. Parent IDs retain recursive ancestry. `valid: false` preserves a previously unresolved or invalid Chrome stack chain without inventing ancestors. A frame marked valid must have a complete, acyclic parent chain.

Resolved frame file/line fields feed the Source panel. The event `args` source fields remain available for existing Chrome imports.

`processes` preserves display metadata as `pid`, `name`, `sort_index`, and `threads`. Each thread has `tid`, `name`, and `sort_index`. These display records do not replace process-lifetime identities.

`counters` contains `pid`, `name`, `unit`, and `points`, each point being `[timestamp, value]`. A missing original counter unit is the empty string. Counter values retain their existing double representation; exact allocation/snapshot byte totals belong to the memory records below. The reader rebuilds flow groups, indexes, ancestry, and other derived model fields rather than saving a second copy.

## Capture and memory records

The `profile` object contains:

| Field | Meaning |
|---|---|
| `source_format`, `source_name`, `converter` | Import provenance strings |
| `capture_start_ts`, `capture_end_ts` | Known coverage boundaries or null |
| `capabilities` | Booleans for `native_cpu_samples`, `managed_cpu_samples`, `native_allocation_history`, `managed_allocation_history`, `managed_heap_snapshots`, and `managed_survival` |
| `quality` | `incomplete_capture`, `sampled_cpu`, `sampled_allocations`, `unresolved_symbols`, optional decimal-string `lost_events`, a string-array `warnings`, and optional `allocation_history_gaps` |
| `process_instances` | `id`, `pid`, optional `start_ts` and `end_ts` |
| `heap_instances` | `id`, `process_id`, optional `start_ts` and `end_ts` |
| `modules` | `id`, `process_id`, `name`, `path`, `build_id`, optional `pdb_path`, `load_address`, `size_bytes`, optional `load_ts` and `unload_ts` |
| `allocations` | Allocation lifetimes described below |
| `managed_snapshots` | Snapshot totals and type summaries described below |
| `managed_survival` | GC observations described below |

A capability is true only when the capture supplies that data. False does not fabricate an empty measurement. Quality warnings explain known gaps, sampling, and unavailable analyses. Capability presence does not establish complete coverage.

Each allocation has `id`, `process_id`, `heap_id`, optional `address`, exact `size_bytes`, `stack_frame_id`, `type_id`, `kind`, optional `allocated_ts` and `freed_ts`, and `end_state`. `kind` is `native` or `managed`. The stack ID is empty when no allocation origin was recorded. A type or retaining reference never supplies an inferred allocation origin. Heap/type IDs can be empty when unknown. A nonempty heap ID must belong to the allocation's process lifetime.

`end_state` is one of:

- `unknown`: the available data does not establish whether the allocation remains live.
- `freed`: release or collection was observed. `freed_ts` is present only for an exact recorded free time. A GC observation can establish death without an exact free timestamp.
- `live_at_capture_end`: recording coverage establishes that the allocation was still live at the end. A missing free event alone is insufficient when coverage is incomplete.

A null `allocated_ts` means the allocation start was not observed. A null `freed_ts` does not imply either live or freed. Reused addresses retain distinct allocation IDs, even inside the same process and heap. Outstanding bytes at a selected time are derived later from these observations and coverage; the file does not store an unexplained aggregate as a lifetime.

Native replay retains the origin recorded at allocation time, including when another thread frees the block. Exact heap destruction and process exit end their outstanding allocations. An unmatched free has no recorded size or origin and produces a coverage warning, not a zero-byte allocation. Reused addresses with a missing free leave the prior allocation's end unknown. A repeated heap creation bounds the earlier heap generation without inventing its allocations' free timestamps.

A moved realloc represented by `Alloc(new)`, `Free(old)`, and a summary retains the new allocation's original timestamp and stack. The summary does not replay either action. An in-place resize ends the old generation and starts a generation with the recorded new size and realloc stack. A null result leaves the original allocation unchanged. Moved summaries without their allocation/free pair remain a coverage gap; the importer does not invent allocation origins or release times from them.

`TraceModel::query_outstanding_memory(T, born_between, process_id)` derives native totals from an index rebuilt by `build_index()`. An exact lifetime contributes when `allocated_ts <= T < freed_ts`. Known live-at-end records contribute through the capture end, inclusive. Queries outside a known capture interval return an error. The optional birth range is `[start, end)` and selects allocations born in that range which are outstanding at `T`; it is not a difference of two outstanding totals. Allocations without a birth timestamp cannot satisfy the range and are counted separately as omitted unknown births.

Results separate known bytes/counts from uncertain candidates. Unknown ends or births remain uncertain. Reported event loss, allocation sampling, or `allocation_history_gaps` makes all candidate totals uncertain. The gap flag records missing relevant events within the history, including lost buffers without a known event count or skipped unsupported heap records. Recorded allocation/free facts remain stored. The incomplete flag and capture warnings remain visible even when individual recorded lifetimes are known. A late start can leave the initial heap unknown without setting the internal-gap flag; the known list is then partial. Uncertain candidates are not a claim that those bytes are still allocated. Summation overflow returns an error and empty totals.

Each query also groups by the original allocation stack and by function within a process lifetime. Function identity uses the module-scoped symbol, then module/address, then the opaque frame ID; display names never identify a native function. Function totals include each allocation once per function in its callpath, even under recursion. Exclusive totals charge only the allocating leaf. Missing stacks form an explicit unattributed group. These results describe recorded outstanding allocations, including deliberate retention, without classifying leaks or inferring retaining references.

Each managed snapshot has `id`, `process_id`, `ts`, optional `live_bytes` and `object_count`, and `types`. Each type summary has `type_id`, `name`, exact `object_count`, and exact `size_bytes`. Snapshot type totals describe observed live objects; differences are not allocation activity.

Each managed survival record has `allocation_id`, `ts`, `survived`, and optional `address`. It references a managed allocation, records the GC observation time, and can retain an address after relocation. `survived: false` is an observation of collection, not an exact free time. Object references and retention graphs are outside version 1.

Native module `build_id` uses the recorded PDB GUID and decimal age as `GUID/age`. The optional `pdb_path` records the RSDS path; it defaults to empty when reading earlier version 1 profiles. Symbol lookup searches its basename only within supplied local directories. It never trusts the recorded path as proof of the captured build.
