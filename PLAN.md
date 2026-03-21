# TraceRender — Architecture & Design

## Overview

TraceRender is a from-scratch Chrome JSON trace viewer built with C++17, Dear ImGui (docking branch), SDL3, OpenGL, and SQLite. It runs natively on desktop (Linux, Windows, macOS) and in the browser via WebAssembly/Emscripten. The viewer handles large traces (100MB+) at 60fps through SAX streaming parsing, string interning, spatial indexing, and sub-pixel culling.

## Architecture

### Layer Diagram

```
┌─────────────────────────────────────────────────────────┐
│  main.cpp          Entry point, SDL3/OpenGL init,       │
│                    event loop, ImGui setup               │
├─────────────────────────────────────────────────────────┤
│  App               Shell: dockspace layout, panel       │
│                    orchestration, settings, file loading │
├──────────┬──────────────────────────────┬───────────────┤
│ Platform │         UI Layer             │    Tracing    │
│          │                              │               │
│ SDL3/GL  │  TimelineView  DetailPanel   │  Self-profile │
│ File I/O │  SearchPanel   FilterPanel   │  Chrome JSON  │
│ WASM     │  StatsPanel    FlameGraph    │  output       │
│ Memory   │  InstancePanel SourcePanel   │               │
│          │  Diagnostics   Toolbar       │               │
│          │  CounterTrack  FlowRenderer  │               │
│          │  RangeStats    KeyBindings   │               │
├──────────┴──────────┬───────────────────┴───────────────┤
│                     │                                    │
│  Parser             │  Model                             │
│  SAX JSON handler   │  TraceModel, TraceEvent, ViewState │
│  B/E pair matching  │  BlockIndex, QueryDb, ColorPalette │
│                     │                                    │
└─────────────────────┴────────────────────────────────────┘
```

### Directory Structure

```
src/
├── main.cpp                  SDL3/OpenGL init, ImGui setup, main loop
├── app.h / app.cpp           App shell: owns all panels, model, view state
├── tracing.h                 RAII self-profiling tracer (Chrome JSON output)
│
├── model/                    Data layer
│   ├── trace_event.h         TraceEvent (~40 bytes), Phase enum (17 phases)
│   ├── trace_model.h/cpp     Central container: events, string pool, processes,
│   │                         threads, counters, flows, indexes, call stack nav
│   ├── block_index.h         256-event block spatial index (O(log N) range queries)
│   ├── color_palette.h       48-color hash-based category/name coloring
│   └── query_db.h/cpp        SQLite in-memory DB with async execution
│
├── parser/
│   └── trace_parser.h/cpp    SAX streaming JSON parser, B/E matching, indexing
│
├── platform/                 Compile-time platform switching
│   ├── platform.h            Abstract interface (GL, dialogs, main loop, settings)
│   ├── platform_desktop.cpp  SDL3/OpenGL 3.3, native file dialogs, ~/.config persistence
│   ├── platform_wasm.cpp     Emscripten/WebGL2, JS interop, browser file I/O
│   ├── file_loader.h         Async loading interface (4-phase progress)
│   ├── file_loader_desktop.cpp  Background std::thread loader
│   ├── file_loader_wasm.cpp     Synchronous loader (no threads)
│   └── memory.h              RSS memory query (Linux /proc, macOS task_info)
│
├── ui/                       ImGui panels and rendering
│   ├── view_state.h          Viewport, selection, filters, layout, key bindings
│   ├── timeline_view.h/cpp   Ruler, tracks, event boxes, zoom/pan, hit testing
│   ├── detail_panel.h/cpp    Call stack, children breakdown, arguments tree
│   ├── search_panel.h/cpp    Text search, name stats, unique-by-name filter
│   ├── filter_panel.h/cpp    Process/thread/category visibility toggles
│   ├── stats_panel.h/cpp     SQL editor, visual query builder, result tables
│   ├── flame_graph_panel.h/cpp  Per-thread icicle charts, flat node pool
│   ├── instance_panel.h/cpp  Function instance list with keyboard nav
│   ├── source_panel.h/cpp    Source viewer with path remapping
│   ├── diagnostics_panel.h/cpp  FPS/memory sparklines, render stats
│   ├── counter_track.h/cpp   Step-function graphs with sub-pixel merging
│   ├── flow_renderer.h/cpp   Bezier arrows for flow/async events
│   ├── toolbar.h/cpp         Menu bar: file open, zoom, memory, settings
│   ├── range_stats.h/cpp     Aggregated stats for selected time ranges
│   ├── key_bindings.h/cpp    Rebindable shortcuts (17 actions, primary+alt)
│   ├── format_time.h         Auto-scaling time display (ns/us/ms/s)
│   ├── export_utils.h        CSV/TSV export (RFC 4180)
│   ├── string_utils.h        Case-insensitive substring search
│   └── sort_utils.h          Three-way comparators for ImGui tables
│
tests/                        Google Test unit tests (15 files)
scripts/                      Build/test/format wrapper scripts
web/                          WebAssembly HTML shell
```

## Data Model

### TraceEvent (~40 bytes)

The fundamental unit. All string data is interned; events store indices, not copies.

| Field | Type | Description |
|-------|------|-------------|
| `name_idx` | `uint32_t` | Index into string pool |
| `cat_idx` | `uint32_t` | Category index into string pool |
| `ph` | `Phase` | Event phase (B/E/X/i/C/s/t/f/M/b/e/n/N/O/D/P/R) |
| `ts` | `double` | Timestamp in microseconds |
| `dur` | `double` | Duration in microseconds |
| `pid` | `uint32_t` | Process ID |
| `tid` | `uint32_t` | Thread ID |
| `id` | `uint64_t` | Flow/async event ID |
| `args_idx` | `uint32_t` | Index into args storage (UINT32_MAX = none) |
| `depth` | `uint8_t` | Nesting depth (computed in build_index) |
| `is_end_event` | `bool` | True for matched 'E' events (filtered after indexing) |
| `parent_idx` | `int32_t` | Parent event index (-1 if root, computed in build_index) |
| `self_time` | `double` | Wall time minus children (computed in build_index) |

### TraceModel

Central container owning all trace data:

- **`events_`** — Flat vector of all TraceEvent objects
- **`strings_`** — Interned string pool (names, categories)
- **`string_map_`** — Hash map for deduplication during parsing
- **`args_`** — Raw JSON strings for event arguments (lazy deserialization)
- **`processes_`** — ProcessInfo hierarchy, each containing ThreadInfo vector
- **`counter_series_`** — Counter tracks: `(timestamp, value)` pairs per series
- **`flow_groups_`** — Map from flow ID to event indices
- **`categories_`** — Unique sorted category indices
- **`name_to_events_`** — Map from name_idx to sorted event indices

### Hierarchy

```
TraceModel
├── events[]                  Flat array, all events across all threads
├── strings[]                 Interned string pool
├── args[]                    JSON argument strings
├── processes[]
│   └── ProcessInfo           pid, name, sort_index
│       └── threads[]
│           └── ThreadInfo    tid, name, sort_index, max_depth
│               ├── event_indices[]   Sorted by (ts, -dur)
│               └── block_index       Spatial index for range queries
├── counter_series[]          (pid, name) → (timestamp, value) pairs
├── flow_groups{}             flow_id → [event_indices]
├── categories[]              Unique category indices
└── name_to_events{}          name_idx → [event_indices]
```

### ViewState

Shared mutable state passed to all panels by reference:

- **Viewport**: `view_start_ts`, `view_end_ts`, `trace_start_ts`, `trace_end_ts`
- **Selection**: Selected event index, pending scroll flag
- **Range**: Start/end timestamps, drag state
- **Filters**: Hidden pid/tid/category sets
- **Search**: Query string, result indices, current index
- **Layout**: Track height, label width, ruler height, counter height, flame bar settings
- **Rendering**: Flow arrow visibility, selection border color, time unit (ns/us)
- **Coordinate helpers**: `time_to_x()`, `x_to_time()`, `zoom_to_fit()`, `navigate_to_event()`

## Parsing Pipeline

### SAX Streaming Parser

Uses nlohmann/json SAX handler to process traces without building a DOM tree:

1. **Read file** — 4MB chunked reads with progress reporting
2. **SAX parse** — State machine: TopLevel → InTopObject → InTraceEvents → InEvent → InArgs
3. **Event extraction** — Fields mapped to TraceEvent, strings interned, args reconstructed as JSON
4. **Special handling**:
   - Counter events (C): Split into separate CounterSeries per `(pid, arg_key)`
   - Flow events (s/t/f): Grouped by ID
   - Metadata (M): Set process/thread names and sort indices
   - Duration B/E: Stored as-is, matched later in build_index

### build_index() — Post-Parse Indexing

All derived data computed once, stored on events/threads:

1. **Sort** — Events per thread sorted by `(ts, -dur)` (parents before children)
2. **Match B/E pairs** — Stack-based: 'B' pushes, 'E' pops and computes duration
3. **Per-thread processing**:
   - Remove matched end events and metadata
   - Deduplicate same-name/same-timestamp events
   - Compute nesting depth via stack of active spans
   - Set `parent_idx` from stack
   - Compute `self_time = dur - sum(immediate_children.dur)`
   - Build BlockIndex (256-event blocks with min_ts, max_end_ts, depth_mask)
4. **Global state** — Sort processes/threads, collect categories, build name→events map, compute counter min/max, cache aggregate stats

## Spatial Indexing

### BlockIndex

Events grouped into 256-event blocks for fast visible-range queries:

| Block Field | Purpose |
|------------|---------|
| `min_ts` | Earliest event timestamp in block |
| `max_end_ts` | Latest end timestamp (monotonically propagated) |
| `local_max_end_ts` | Max end within this block only |
| `start_idx` / `count` | Position in event_indices |
| `depth_mask` | Bitmask of depths 0-31 present |

**Query**: Binary search for first block where `max_end_ts >= range_start`, iterate blocks until `min_ts > range_end`. O(log N) block search + O(K) per visible event.

## Rendering Pipeline

### Frame Loop

```
main_loop_step()
├── SDL_PollEvent()           Handle input, quit, file drops
├── ImGui_NewFrame()          Begin ImGui frame
├── App::update()
│   ├── FileLoader::poll()    Check async load completion
│   ├── Dockspace setup       Configure panel layout (first frame)
│   ├── Toolbar::render()     Menu bar
│   ├── Settings modal        If open
│   ├── Panel rendering       If trace loaded:
│   │   ├── TimelineView      Ruler, tracks, events, selection
│   │   ├── DetailPanel       Call stack, children, args
│   │   ├── SearchPanel       Search results table
│   │   ├── FilterPanel       Visibility checkboxes
│   │   ├── StatsPanel        SQL queries
│   │   ├── InstancePanel     Function instances
│   │   ├── SourcePanel       Source code
│   │   ├── FlameGraphPanel   Icicle charts
│   │   └── DiagnosticsPanel  FPS/memory sparklines
│   ├── Welcome screen        If no trace
│   ├── Loading overlay       If loading (spinner + progress bar)
│   └── Status bar            Selected event, load progress
├── ImGui::Render()           Process draw commands
├── glClear + RenderDrawData  OpenGL rendering
└── SDL_GL_SwapWindow()       Present frame
```

### Timeline Rendering

- **ImDrawList** for direct draw commands (no ImGui widgets)
- **Sub-pixel culling**: Events < 2px wide merged into per-depth gray bars
- **Clip regions**: Per-thread clip rects prevent overdraw
- **Text culling**: Labels only drawn when event box is wide enough
- **Instant events**: Rendered as diamond shapes
- **Counter tracks**: Step-function line graphs with sub-pixel point merging
- **Flow arrows**: Bezier curves from flow start to flow end across track positions

### Dockspace Layout

```
┌──────────────┬────────────────────────────────┐
│              │                                │
│ Diagnostics  │  Timeline / Source              │
│   (18%)      │        (center)                │
│              │                                │
│              ├─────────────────┬──────────────┤
│              │                 │              │
│              │                 │ Details /    │
│              │                 │ Filters      │
│              │                 │   (35%)      │
├──────────────┴─────────────────┤              │
│                                │              │
│ Search / Stats / Flame Graph   │              │
│           (bottom 42%)         │              │
│                     ┌──────────┤              │
│                     │Instances │              │
└─────────────────────┴──────────┴──────────────┘
```

## Platform Abstraction

The `platform::` namespace provides a compile-time abstraction:

| Function | Desktop (SDL3/GL 3.3) | WASM (Emscripten/WebGL2) |
|----------|----------------------|--------------------------|
| Main loop | `while (*running) step()` | `emscripten_set_main_loop()` |
| File open | SDL3 native dialog | HTML `<input type="file">` + JS |
| File save | SDL3 save dialog | Browser download via Blob |
| File drop | SDL_EVENT_DROP_FILE | JS interop |
| Settings | `~/.config/trace-render/settings.json` | None |
| Font scale | 1.4x | 1.5x |
| GLSL | `#version 330 core` | `#version 300 es` |
| File loading | Background `std::thread` | Synchronous |
| Memory query | `/proc/self/statm` (Linux) / `task_info` (macOS) | N/A |

## SQL Query System

SQLite in-memory database populated from TraceModel after loading:

```sql
-- Schema
events    (id, name, category, phase, ts, dur, end_ts, pid, tid, depth)
processes (pid, name)
threads   (tid, pid, name)
counters  (pid, name, ts, value)
```

- Async execution on background thread with progress tracking
- Visual query builder (SELECT with aggregates, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT)
- Multiple saved query tabs with JSON persistence
- Sortable result tables with CSV/TSV export

## Self-Profiling

The `Tracer` singleton emits Chrome JSON trace format for profiling the viewer itself:

- `TRACE_SCOPE(name)` / `TRACE_FUNCTION()` — RAII duration events with file/line/func args
- `TRACE_SCOPE_ARGS(name, cat, ...)` — Duration events with user-supplied args
- Counter events for FPS and RSS memory written each frame
- Output trace can be loaded back into the viewer
- Enabled via `--trace output.json` CLI flag

## Testing

15 Google Test files covering all non-UI logic:

| Test File | Coverage |
|-----------|----------|
| `test_parser` | JSON formats, metadata, counters, flows, B/E matching, time units |
| `test_trace_model` | String interning, hierarchy, depth, parent/sibling nav, self-time, dedup |
| `test_trace_event` | Phase mapping, end_ts, defaults |
| `test_block_index` | Range queries, multi-block spans, empty index |
| `test_query_db` | SQL execution, aggregation, joins, error handling |
| `test_view_state` | Coordinate conversion, zoom, range selection, bounds clamping |
| `test_timeline_hit_test` | Category/depth filtering, shortest-duration selection |
| `test_search_panel` | Name stats, unique-by-name, empty results |
| `test_flame_graph_panel` | Tree building, aggregation, range/filter scoping, deep nesting |
| `test_counter_track` | Value lookup, point merging, zoom scenarios |
| `test_source_panel` | Location extraction, path remapping (Windows/WSL/Jenkins) |
| `test_format_time` | All units, boundaries, negative values, ruler formatting |
| `test_export_utils` | CSV/TSV with RFC 4180 quoting |
| `test_tracing` | Arg serialization, escaping, scope macros, integration |
| `test_panel_reset` | Panel lifecycle: `on_model_changed()` resets |

CI runs on Ubuntu (GCC, Clang) and Windows (MSVC) via GitHub Actions.

## Dependencies

All fetched via CMake FetchContent (no system packages except OpenGL):

| Dependency | Version | Purpose |
|-----------|---------|---------|
| Dear ImGui | docking branch | Immediate-mode UI framework |
| SDL3 | 3.2.8 (static) | Windowing, input, OpenGL/WebGL context |
| nlohmann/json | 3.11.3 | SAX streaming JSON parser |
| SQLite | 3.45.1 (amalgamation) | In-memory SQL query engine |
| Google Test | 1.15.2 | Unit testing (desktop only) |

## Key Design Principles

1. **Pre-compute in build_index()** — Depth, parent indices, self times, categories, name maps all computed once. The render loop does lookups, not scans.
2. **String interning** — All names and categories stored once in a shared pool. Events reference 4-byte indices, not string copies.
3. **SAX streaming** — Never build a DOM tree. Parse events one at a time for bounded memory usage on large traces.
4. **Lazy deserialization** — Event arguments stored as raw JSON strings, only parsed when displayed in the detail panel.
5. **Spatial indexing** — BlockIndex enables O(log N) visible-range queries per thread, with depth bitmasks for filtered queries.
6. **ImDrawList rendering** — Bypass ImGui widget overhead. Direct draw commands for thousands of colored rectangles at 60fps.
7. **Sub-pixel culling** — Events narrower than ~2px merged into per-depth gray bars to prevent overdraw and GPU waste.
8. **Flat data structures** — Flame graph uses index-based node pools (not pointers) for cache-friendly traversal and safe rebuilds.
9. **Deferred expensive work** — Range statistics computed when drag ends, not during. SQL queries run async with progress.
10. **Platform abstraction** — Desktop and WASM share all business logic; only windowing/file I/O differs via compile-time switching.
