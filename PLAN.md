# Chrome Trace Viewer - C++ / ImGui

## Context
Build a from-scratch Chrome JSON trace viewer using C++ and Dear ImGui (SDL3 + OpenGL backend, CMake build system). The viewer will support the full Chrome trace event format with timeline visualization, detail inspection, search, filtering, counter tracks, and flow arrows.

## Architecture

### Directory Structure
```
perfetto-imgui/
  CMakeLists.txt
  src/
    main.cpp                 # SDL3/OpenGL init, main loop
    app.h / app.cpp          # App class, DockSpace, orchestration
    model/
      trace_event.h          # TraceEvent struct, Phase enum
      trace_model.h / .cpp   # TraceModel: processes, threads, string pool, indexes
      block_index.h          # Block-based spatial index for visible-range queries
      color_palette.h        # Hash-based category-to-color mapping
    parser/
      trace_parser.h / .cpp  # SAX-based JSON parser -> TraceModel
    ui/
      timeline_view.h / .cpp # ImDrawList-based timeline rendering + zoom/pan/select
      detail_panel.h / .cpp  # Selected event metadata display
      search_panel.h / .cpp  # Search by name/category
      filter_panel.h / .cpp  # Process/thread/category filter checkboxes
      counter_track.h / .cpp # Counter event line charts
      flow_renderer.h / .cpp # Flow arrow bezier curves
      toolbar.h / .cpp       # File open, zoom controls
```

### Dependencies (all via CMake FetchContent)
- **Dear ImGui** (docking branch) - UI framework
- **SDL3** (static) - windowing/input/OpenGL context
- **nlohmann/json** - SAX JSON parsing
- **OpenGL** - system package

### Key Design Decisions
1. **SAX parser** (not DOM) - essential for large traces (100MB+), processes events one-at-a-time instead of building full JSON tree in memory
2. **String pooling** - intern event names/categories into a shared table, events store 4-byte indices instead of string copies
3. **Args as raw JSON strings** - only deserialize when displaying in detail panel (one event at a time)
4. **Block-based spatial index** - events sorted by timestamp, divided into blocks of 256; each block stores min_ts/max_end_ts for fast range skip. Simpler than interval tree, great cache locality
5. **ImDrawList for timeline** - direct draw commands (AddRectFilled, AddText) instead of ImGui widgets for 10,000+ visible slices
6. **Sub-pixel culling** - slices narrower than 1px rendered as thin lines to prevent overdraw
7. **TraceEvent struct ~40 bytes** - compact for cache-friendly iteration

### Data Model
- `TraceEvent` - name_idx, cat_idx, ph, ts, dur, pid, tid, id, args_idx (all indices into pools)
- `TraceModel` - events vector, string pool, args pool, process/thread hierarchy, counter series, flow groups, block indexes
- `ViewState` - shared mutable state: viewport (view_start_ts/view_end_ts), selection, hidden pids/tids/cats, search state
- `ProcessInfo` / `ThreadInfo` - name, sort_index, event indices

### Parser
- nlohmann/json SAX handler with state machine: TopLevel -> InTraceEvents -> InEvent -> InArgs
- Handles both `[...]` array and `{"traceEvents": [...]}` object formats
- Post-parse `build_index()`: match B/E pairs, compute nesting depths, sort, build block indexes

### Timeline Rendering
- **Label gutter** (left ~200px): process/thread names
- **Time ruler** (top ~30px): adaptive tick marks (1us to 10s intervals)
- **Track area**: custom-drawn colored rectangles per event, text labels when wide enough
- **Zoom**: mouse wheel centered on cursor position (20% per notch)
- **Pan**: middle-click drag (horizontal + vertical)
- **Selection**: left-click hit testing by position -> depth -> time interval

## Implementation Order

### Phase 1: Skeleton App
1. Create CMakeLists.txt with FetchContent for all deps
2. Create main.cpp: SDL3+OpenGL init, ImGui setup with docking, main loop showing demo window
3. Create app.h/.cpp: DockSpace with placeholder windows

### Phase 2: Data Model & Parser
4. Implement trace_event.h (TraceEvent, Phase enum)
5. Implement trace_model.h/.cpp (TraceModel with string pool, process/thread hierarchy)
6. Implement trace_parser.h/.cpp (SAX handler, metadata events, args capture)
7. Implement build_index() (B/E matching, depth computation, sorting, time range)

### Phase 3: Basic Timeline
8. Implement color_palette.h
9. Implement ViewState with time<->pixel helpers
10. Implement toolbar.h/.cpp (file open)
11. Implement timeline_view.h/.cpp: time ruler, track headers, colored slices for X events
12. Add zoom (mouse wheel) and pan (middle-click drag)
13. Add sub-pixel culling and vertical scrolling

### Phase 4: Spatial Index & Selection
14. Implement block_index.h and integrate into query_visible()
15. Implement click-to-select with hit testing
16. Implement detail_panel.h/.cpp (selected event metadata + parsed args)

### Phase 5: Search & Filtering
17. Implement search_panel.h/.cpp (text search, results list, click-to-navigate)
18. Implement filter_panel.h/.cpp (process/thread/category checkboxes)

### Phase 6: Counter Tracks
19. Handle Phase::Counter in parser, build counter_series_
20. Implement counter_track.h/.cpp (line charts with AddLine)

### Phase 7: Flow Arrows
21. Handle flow events in parser, build flow_groups_
22. Implement flow_renderer.h/.cpp (bezier curves between slices)

### Phase 8: Polish
23. Native file dialog (SDL3 dialog API or tinyfiledialogs)
24. Loading progress bar
25. Hover tooltips
26. Keyboard shortcuts (WASD zoom/pan, Ctrl+F search, F fit, Esc deselect)
27. Edge case handling (empty traces, malformed JSON)

## Verification
1. Build: `cmake -B build && cmake --build build`
2. Run: `./build/perfetto_imgui`
3. Open a Chrome trace file (record from `chrome://tracing` or use a sample)
4. Verify: timeline shows colored slices, zoom/pan works, clicking shows details
5. Test with a large trace (50MB+) to verify performance (should maintain 60fps)
