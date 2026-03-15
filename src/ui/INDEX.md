# src/ui/
All ImGui panels and rendering components. Each panel has a `render(model, view)` method. Shared state flows through `ViewState`.

## view_state.h — shared viewport + interaction state passed by ref to every render call
```
void set_range_selection(double start, double end);
void clear_range_selection();
ImU32 sel_border_color_u32() const;
float time_to_x(double ts, float timeline_left, float timeline_width) const;
double x_to_time(float x, float timeline_left, float timeline_width) const;
void zoom_to_fit(double min_ts, double max_ts);
void navigate_to_event(int32_t ev_idx, const TraceEvent& ev, double pad_factor = 0.5, double min_pad_us = 100.0);
uint32_t filter_generation;  // bumped on any hidden_pids/tids/cats change
```

## timeline_view.h / timeline_view.cpp — main timeline: ruler, tracks, event boxes, zoom/pan, range selection
```
void render(const TraceModel&, ViewState&);
DiagStats diag_stats;  // written each frame, read by DiagnosticsPanel
```

## detail_panel.h / detail_panel.cpp — selected-event details: timing, args, call stack, children table, range summary
```
void render(const TraceModel&, ViewState&);
```

## search_panel.h / search_panel.cpp — text search over event names; populates `ViewState::search_results`
```
void render(const TraceModel&, ViewState&);
```

## filter_panel.h / filter_panel.cpp — hide/show processes, threads, categories via `ViewState::hidden_*`
```
void render(const TraceModel&, ViewState&);
```

## stats_panel.h / stats_panel.cpp — SQL editor + result table + visual query builder; tabs serialized to JSON
```
void render(const TraceModel&, QueryDb&, ViewState&);
nlohmann::json save_tabs() const;
void load_tabs(const nlohmann::json&);
// QueryBuilderState
void reset();
std::string build_sql(const char* const* columns, int num_columns) const;
```

## instance_panel.h / instance_panel.cpp — lists all instances of the selected function; keyboard navigation
```
void render(const TraceModel&, ViewState&);
```

## diagnostics_panel.h / diagnostics_panel.cpp — FPS / memory sparklines and per-frame render stats
```
void render(const TraceModel&, const ViewState&);
float current_rss_mb() const;
DiagStats stats;  // fields: visible_slices, drawn_slices, merged_slices, merge_runs, labels_drawn, tracks_visible, instant_events
```

## source_panel.h / source_panel.cpp — shows source file for selected event; supports path prefix remapping
```
bool extract_source_location(const TraceModel&, const TraceEvent&, std::string& file, int& line);
std::string remap_source_path(const std::string& trace_path, const std::string& strip_prefix, const std::string& local_base);
void render(const TraceModel&, ViewState&);
nlohmann::json save_settings() const;
void load_settings(const nlohmann::json&);
```

## flamegraph_panel.h / flamegraph_panel.cpp — aggregated flame graph: merges call stacks by (name, category), click-to-zoom, icicle toggle
```
void render(const TraceModel&, ViewState&);
void rebuild(const TraceModel&, const ViewState&);  // exposed for testing
const std::vector<FlameNode>& nodes() const;        // exposed for testing
size_t root() const;                                // exposed for testing
```

## counter_track.h / counter_track.cpp — renders counter series as step-function graphs; sub-pixel merging + hover hit-test
```
bool counter_lookup_value(const CounterSeries&, double time, double& out_timestamp, double& out_value);
std::vector<MergedCounterSegment> merge_counter_points(const std::vector<std::pair<double,double>>&,
    double view_start, double view_end, float track_x, float track_w);
float render(ImDrawList*, ImVec2 area_min, float y_offset, float width, const TraceModel&, uint32_t pid, const ViewState&);
void render_series(ImDrawList*, ImVec2 track_min, ImVec2 track_max, const CounterSeries&, const ViewState&, ImU32 color);
bool hit_test(float mouse_x, float mouse_y, const ViewState&, CounterHitResult& result) const;
```

## flow_renderer.h / flow_renderer.cpp — bezier arrows connecting flow/async events across tracks
```
void set_track_positions(const std::unordered_map<uint64_t, TrackPos>&);
void render(ImDrawList*, const TraceModel&, const ViewState&, ImVec2 area_min, ImVec2 area_max, float label_width);
static uint64_t make_key(uint32_t pid, uint32_t tid);
```

## toolbar.h / toolbar.cpp — open-file button, zoom controls, time unit toggle, memory readout, settings button
```
void render(const TraceModel&, ViewState&, float rss_mb);
void set_window(SDL_Window*);
bool settings_requested() const;
void clear_settings_request();
```

## range_stats.h / range_stats.cpp — per-name stats (count, total/min/max/avg dur) for events in a time range
```
RangeStats compute_range_stats(const TraceModel&, double start_ts, double end_ts);
double RangeEventSummary::avg_dur() const;
```

## format_time.h — time display helpers (always use instead of inline formatting)
```
void format_time(double us, char* buf, size_t buf_size);
void format_ruler_time(double us, double tick_interval, char* buf, size_t buf_size);
```

## string_utils.h — case-insensitive substring search
```
bool contains_case_insensitive(const std::string& haystack, const std::string& needle);
```

## sort_utils.h — three-way comparator for ImGui table sort callbacks
```
template <typename T> int sort_utils::three_way_cmp(const T& a, const T& b);  // -1 / 0 / 1
```
