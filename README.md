# TraceRender

A fast, native Chrome trace viewer built with C++ and [Dear ImGui](https://github.com/ocornut/imgui). Also runs in the browser via WebAssembly.

![Timeline with diagnostics, source viewer, and SQL query](screenshots/1.png)
![Source code viewer with syntax highlighting](screenshots/2.png)
![Search results and children breakdown](screenshots/3.png)

## Features

### Visualization
- **Timeline** — Zoomable/pannable timeline with colored slices per process/thread, sub-pixel culling, and collapsible process sections
- **Flame graph** — Per-thread icicle chart with zoom/breadcrumb navigation, range scoping, search highlighting, and selection sync
- **Counter tracks** — Auto-scaled step-function line charts for counter events with hover tooltips and sub-pixel point merging
- **Flow arrows** — Bezier curves connecting flow events across threads (toggleable)

### Inspection
- **Detail panel** — Tabbed inspector with Call Stack (collapsible descendant tree with wall/self/child time), Children breakdown (aggregated count, total, avg, min, max with heat-colored bars), and Arguments view
- **Range selection** — Click+drag on ruler or Shift+drag in tracks to select a time range with aggregated statistics
- **Instance browser** — List and navigate all instances of a selected function
- **Source viewer** — Jump to source code for traced functions with syntax highlighting, selectable/copyable text, and configurable path remapping for CI/cross-platform builds

### Search & Query
- **Search** — Case-insensitive search by event name or category with sortable results table, count/average columns, and unique-by-name deduplication
- **SQL queries** — Query trace events with SQLite, visual query builder with aggregate functions, schema inspector, multiple saved query tabs, and sortable results with async execution
- **Statistics** — Per-function aggregated timing with count, total, avg, min, max; CSV/TSV export

### Navigation & Controls
- **Filtering** — Toggle visibility of processes, threads, and categories via tree checkboxes
- **Go to time** — Jump to a specific timestamp (G key) supporting ns/us/ms/s units
- **Configurable keyboard shortcuts** — Rebindable primary and alternate bindings for all actions via Settings dialog
- **Keyboard shortcuts** — WASD call-stack navigation, arrow keys for pan/scroll, F to fit, G to go to time, Escape to deselect

### Application
- **Diagnostics** — Live render stats, FPS sparkline, memory usage history, sub-pixel culling metrics
- **Loading progress** — Four-phase progress display (reading file, parsing JSON, building index, building query DB)
- **Native file dialog** — OS file picker via SDL3, plus drag & drop support
- **Resizable label gutter** — Drag the splitter to resize thread labels
- **Memory counter** — Real-time RSS memory usage displayed in toolbar and as a counter track
- **Themes** — Dark and light themes with configurable font scale, track height, and selection border color
- **Self-profiling** — Emit an internal performance trace of the viewer itself with `--trace`

## Supported Format

[Chrome JSON Trace Event Format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU) — both array (`[...]`) and object (`{"traceEvents": [...]}`) formats.

Supports event phases: X (complete), B/E (duration begin/end), i (instant), C (counter), s/t/f (flow), M (metadata), b/e/n (async), N/O/D (object), P (sample), R (mark).

## Building

### Desktop

Requires CMake 3.22+, a C++17 compiler, and OpenGL development headers.

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libgl-dev

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

All other dependencies (SDL3, Dear ImGui, nlohmann/json, SQLite3) are fetched automatically via CMake FetchContent.

### WebAssembly

Requires [Emscripten](https://emscripten.org/).

```bash
./scripts/build_wasm.sh
```

## Usage

```bash
# Open with file dialog
./build/trace_render

# Open a trace file directly
./build/trace_render trace.json

# Interpret timestamps as nanoseconds (default is microseconds)
./build/trace_render -ns trace.json

# Explicitly interpret timestamps as microseconds
./build/trace_render -us trace.json

# Self-profiling: emit an internal trace of the viewer itself
./build/trace_render --trace output.json trace.json
```

You can also drag & drop a trace file onto the window.

### Controls

| Action | Input |
|--------|-------|
| Zoom in/out | Mouse wheel / =/- keys |
| Pan horizontally | Middle-click drag / Ctrl+left drag / Left/Right arrows |
| Scroll vertically | Shift+mouse wheel / Up/Down arrows |
| Navigate to parent | W |
| Navigate to first child | S |
| Navigate to prev sibling | A |
| Navigate to next sibling | D |
| Select event | Left click |
| Select time range | Click+drag on ruler / Shift+drag in tracks |
| Fit to selection/range | F |
| Fit entire trace | F (with nothing selected) |
| Go to time | G |
| Clear selection | Escape |
| Open file | Ctrl+O |
| Search | Ctrl+F |
| Settings | Ctrl+, |
| Run SQL query | Ctrl+Enter |

All keyboard shortcuts can be customized in Settings > Keyboard.

## Architecture

```
src/
  main.cpp                       Entry point: SDL3/OpenGL init, main loop
  app.h / app.cpp                App shell: dockspace, panel orchestration, settings
  tracing.h                      Self-profiling tracer (Chrome JSON output)

  model/
    trace_event.h                TraceEvent struct (~40 bytes), Phase enum
    trace_model.h / .cpp         Events, string pool, processes/threads, indexes
    block_index.h                256-event block spatial index for range queries
    color_palette.h              48-color hash-based category coloring
    query_db.h / .cpp            SQLite database with async query execution

  parser/
    trace_parser.h / .cpp        SAX streaming JSON parser (handles 100MB+ files)

  platform/
    platform.h                   Platform interface (GL, file dialogs, main loop)
    platform_desktop.cpp         SDL3/OpenGL 3.3 desktop implementation
    platform_wasm.cpp            Emscripten/WebGL2 implementation
    file_loader.h                Async file loading interface
    file_loader_desktop.cpp      Threaded background loader
    file_loader_wasm.cpp         Synchronous loader
    memory.h                     Cross-platform RSS memory query

  ui/
    view_state.h                 Viewport, selection, filters, layout state
    timeline_view.h / .cpp       Time ruler, tracks, event rendering, zoom/pan
    detail_panel.h / .cpp        Call stack, children, arguments inspector
    search_panel.h / .cpp        Text search with sortable results
    filter_panel.h / .cpp        Process/thread/category visibility toggles
    stats_panel.h / .cpp         SQL editor, query builder, result tables
    flame_graph_panel.h / .cpp   Per-thread icicle charts
    instance_panel.h / .cpp      Function instance lister with navigation
    source_panel.h / .cpp        Source code viewer with path remapping
    diagnostics_panel.h / .cpp   FPS/memory sparklines, render stats
    counter_track.h / .cpp       Step-function counter visualization
    flow_renderer.h / .cpp       Bezier flow arrows across threads
    toolbar.h / .cpp             Menu bar: file open, zoom, settings
    range_stats.h / .cpp         Aggregated statistics for time ranges
    key_bindings.h / .cpp        Configurable keyboard shortcut system
    format_time.h                Auto-scaling time display (ns/us/ms/s)
    export_utils.h               CSV/TSV export with RFC 4180 quoting
    string_utils.h               Case-insensitive search
    sort_utils.h                 ImGui table sort comparators
```

### Key Design Decisions

- **SAX parser** (not DOM) — Streams events without building a JSON tree, handles large traces (100MB+)
- **String-interned events** — Names and categories stored once in a pool, events reference by index (~40 bytes per event)
- **Block-based spatial index** — Fast visible-range queries with O(log N + K) performance using 256-event blocks with monotonic max_end_ts propagation
- **Pre-computed derived data** — Parent indices, self times, nesting depths, and category sets computed once during `build_index()`, not per-frame
- **ImDrawList rendering** — Direct draw commands for thousands of slices at 60fps, bypassing ImGui widget overhead
- **Sub-pixel culling** — Slices narrower than 1px merged into thin lines per-depth to prevent overdraw
- **Lazy JSON parsing** — Event arguments stored as raw JSON strings, deserialized on demand one event at a time
- **Async SQL execution** — Non-blocking query execution with progress tracking on a background thread
- **Platform abstraction** — Desktop (SDL3/OpenGL 3.3) and WebAssembly (Emscripten/WebGL2) share the same codebase via compile-time platform switching
- **Flat node pools** — Flame graph uses index-based trees (no pointers) for cache-friendly traversal and safe rebuilds

### Dependencies

All fetched automatically via CMake FetchContent:

| Dependency | Version | Purpose |
|-----------|---------|---------|
| [Dear ImGui](https://github.com/ocornut/imgui) | docking branch | UI framework |
| [SDL3](https://github.com/libsdl-org/SDL) | 3.2.8 | Windowing, input, OpenGL context |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | SAX JSON parsing |
| [SQLite](https://sqlite.org) | 3.45.1 | SQL query engine |
| [Google Test](https://github.com/google/googletest) | 1.15.2 | Unit testing (desktop only) |

## Testing

```bash
./scripts/run_tests.sh
```

Uses Google Test with 15 test files covering: parser, trace model, spatial index, time formatting, viewport state, search, timeline hit testing, trace events, counter tracks, source path remapping, SQL queries, CSV/TSV export, self-profiling tracer, flame graph, and panel reset lifecycle.

## Generating Test Traces

```bash
# Generate a synthetic trace for stress testing
python3 scripts/gen_trace.py > trace.json
```

## License

MIT
