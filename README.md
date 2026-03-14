# TraceRender

A fast, native Chrome trace viewer built with C++ and [Dear ImGui](https://github.com/ocornut/imgui). Also runs in the browser via WebAssembly.

![Timeline with diagnostics, source viewer, and SQL query](screenshots/1.png)
![Source code viewer with syntax highlighting](screenshots/2.png)
![Search results and children breakdown](screenshots/3.png)

## Features

- **Timeline** — Zoomable/pannable timeline with colored slices per process/thread, sub-pixel culling, and collapsible process sections
- **Detail panel** — Tabbed inspector with Call Stack (collapsible descendant tree with wall/self/child time), Children breakdown (aggregated count, total, avg, min, max with heat-colored bars), and Arguments view
- **Range selection** — Click+drag on ruler or Shift+drag in tracks to select a time range with aggregated statistics
- **Source viewer** — Jump to source code for traced functions with syntax highlighting, selectable/copyable text, and configurable path remapping for CI/cross-platform builds
- **SQL queries** — Query trace events with SQLite, visual query builder with aggregate functions, schema inspector, multiple saved query tabs, and sortable results with async execution
- **Statistics** — Per-function aggregated timing with count, total, avg, min, max
- **Search** — Case-insensitive search by event name or category with sortable results table and navigation
- **Instance browser** — List and navigate all instances of a selected function
- **Filtering** — Toggle visibility of processes, threads, and categories via tree checkboxes
- **Counter tracks** — Auto-scaled step-function line charts for counter events with hover tooltips and sub-pixel point merging
- **Memory counter** — Real-time RSS memory usage displayed in toolbar and as a counter track
- **Flow arrows** — Bezier curves connecting flow events across threads (toggleable)
- **Go to time** — Jump to a specific timestamp (G key) supporting ns/us/ms/s units
- **Diagnostics** — Live render stats, FPS sparkline, memory usage history, sub-pixel culling metrics
- **Loading progress** — Three-phase progress display (reading file, parsing JSON, building index)
- **Native file dialog** — OS file picker via SDL3, plus drag & drop support
- **Resizable label gutter** — Drag the splitter to resize thread labels
- **Keyboard shortcuts** — WASD zoom/pan, arrow keys, F to fit, G to go to time, Escape to deselect
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

# Self-profiling: emit an internal trace of the viewer itself
./build/trace_render --trace output.json trace.json
```

You can also drag & drop a trace file onto the window.

### Controls

| Action | Input |
|--------|-------|
| Zoom in/out | Mouse wheel / W/S keys |
| Pan horizontally | Middle-click drag / Ctrl+left drag / A/D / Left/Right arrows |
| Scroll vertically | Shift+mouse wheel / Up/Down arrows |
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

## Architecture

- **SAX-based JSON parser** — Streams events without building a DOM, handles large traces (100MB+)
- **String-interned events** — Names and categories stored once in a pool, events reference by index (~40 bytes per event)
- **Block-based spatial index** — Fast visible-range queries with O(log N + K) performance using 256-event blocks
- **Pre-computed derived data** — Parent indices and self times computed once during indexing, not per-frame
- **ImDrawList rendering** — Direct draw commands for thousands of slices at 60fps
- **Sub-pixel culling** — Slices narrower than 1px merged into thin lines to prevent overdraw
- **Lazy JSON parsing** — Event arguments deserialized on demand, one event at a time
- **Async SQL execution** — Non-blocking query execution with progress tracking
- **Platform abstraction** — Desktop (SDL3/OpenGL) and WebAssembly (Emscripten) share the same codebase

## Testing

```bash
./scripts/run_tests.sh
```

Uses Google Test. Tests cover the parser, trace model, spatial index, time formatting, viewport state, source path remapping, counter track rendering, and SQL queries.

## Generating Test Traces

```bash
# Generate a synthetic trace for stress testing
python3 scripts/gen_trace.py > trace.json
```

## License

MIT
