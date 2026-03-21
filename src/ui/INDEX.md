# src/ui/
All ImGui panels and rendering components. Each panel has a `render(model, view)` method. Shared state flows through `ViewState`.

## view_state.h — shared viewport + interaction state passed by ref to every render call (class with getters/setters)
```
// Viewport
double view_start_ts() const; void set_view_start_ts(double); double view_end_ts() const; void set_view_end_ts(double);
void set_view_range(double start, double end);
// Selection
int32_t selected_event_idx() const; void set_selected_event_idx(int32_t);
int32_t pending_scroll_event_idx() const; void set_pending_scroll_event_idx(int32_t);
// Range selection
bool has_range_selection() const; bool range_selecting() const; void set_range_selecting(bool);
double range_start_ts() const; double range_end_ts() const;
void set_range_selection(double start, double end);
void clear_range_selection();
// Filtering
const std::unordered_set<uint32_t>& hidden_pids() const; void hide_pid(uint32_t); void show_pid(uint32_t);
const std::unordered_set<uint32_t>& hidden_tids() const; void hide_tid(uint32_t); void show_tid(uint32_t);
const std::unordered_set<uint32_t>& hidden_cats() const; void hide_cat(uint32_t); void show_cat(uint32_t);
void clear_hidden_pids(); void clear_hidden_tids(); void clear_hidden_cats();
// Search
const std::string& search_query() const; void set_search_query(const std::string&); void clear_search_query();
const std::vector<uint32_t>& search_results() const; void set_search_results(std::vector<uint32_t>);
void add_search_result(uint32_t); void clear_search_results();
int32_t search_current() const; void set_search_current(int32_t);
// Layout defaults
static constexpr float kDefaultTrackHeight, kDefaultTrackPadding, kDefaultCounterTrackHeight;
static constexpr float kDefaultLabelWidth, kDefaultRulerHeight, kDefaultProcHeaderHeight, kDefaultScrollbarScale;
static constexpr float kDefaultFlameBarHeight, kDefaultFlameBarGap;
// Layout
float track_height() const; void set_track_height(float);
float track_padding() const; void set_track_padding(float);
float counter_track_height() const; void set_counter_track_height(float);
float label_width() const; void set_label_width(float);
float ruler_height() const; void set_ruler_height(float);
float proc_header_height() const; void set_proc_header_height(float);
float scrollbar_scale() const; void set_scrollbar_scale(float);
float flame_bar_height() const; void set_flame_bar_height(float);
float flame_bar_gap() const; void set_flame_bar_gap(float);
void reset_layout_defaults();
// Rendering defaults
static constexpr bool kDefaultShowFlows; static constexpr std::array<float, 4> kDefaultSelBorderColor;
void reset_rendering_defaults();
// Selection border color
const std::array<float, 4>& sel_border_color() const; void set_sel_border_color(const std::array<float, 4>&);
ImU32 sel_border_color_u32() const;
// Key bindings
KeyBindings& key_bindings(); const KeyBindings& key_bindings() const;
// Misc
bool show_flows() const; void set_show_flows(bool);
bool time_unit_ns() const; void set_time_unit_ns(bool);
// Coordinate conversion
float time_to_x(double ts, float timeline_left, float timeline_width) const;
double x_to_time(float x, float timeline_left, float timeline_width) const;
void set_trace_bounds(double min_ts, double max_ts);
void zoom_to_fit(double min_ts, double max_ts);
void navigate_to_event(int32_t ev_idx, const TraceEvent& ev, double pad_factor = 0.5, double min_pad_us = 100.0);  // min_pad_us scaled by 1/1000 when time_unit_ns
```

## timeline_view.h / timeline_view.cpp — main timeline: ruler, tracks, event boxes, zoom/pan, range selection
```
void render(const TraceModel&, ViewState&);
DiagStats diag_stats;  // written each frame, read by DiagnosticsPanel
static int32_t select_best_candidate(const std::vector<uint32_t>& candidates, const std::vector<TraceEvent>& events, const std::unordered_set<uint32_t>& hidden_cats, int clicked_depth, double click_time, double tolerance);
```

## detail_panel.h / detail_panel.cpp — selected-event details: timing, args, call stack, children table, range summary
```
void render(const TraceModel&, ViewState&);
void on_model_changed();
```

## search_panel.h / search_panel.cpp — text search over event names; populates `ViewState::search_results`; shows per-name Count and Avg duration
```
void render(const TraceModel&, ViewState&);
void on_model_changed();
void build_name_stats(const TraceModel&, const std::vector<uint32_t>& results);
const std::unordered_map<uint32_t, NameStats>& name_stats() const;
// NameStats: count, total_dur, avg_dur
```

## filter_panel.h / filter_panel.cpp — hide/show processes, threads, categories via `ViewState::hidden_*`
```
void render(const TraceModel&, ViewState&);
```

## stats_panel.h / stats_panel.cpp — SQL editor + result table + visual query builder; tabs serialized to JSON; CSV/TSV export
```
void render(const TraceModel&, QueryDb&, ViewState&);
void set_window(SDL_Window*);
nlohmann::json save_tabs() const;
void load_tabs(const nlohmann::json&);
// QueryBuilderState
void reset();
std::string build_sql(const char* const* columns, int num_columns) const;
```

## export_utils.h — export query results to CSV or TSV format
```
std::string export_result(const QueryDb::QueryResult&, char delimiter);
```

## flame_graph_panel.h / flame_graph_panel.cpp — per-thread icicle charts with flat node pool; filterable sidebar, zoom, search highlighting, context menu
```
void render(const TraceModel&, ViewState&);
void on_model_changed();
void rebuild(const TraceModel&, const ViewState&);
const std::vector<FlameTree>& trees() const;
static int32_t find_longest_instance(const TraceModel&, uint32_t pid, uint32_t tid, uint32_t name_idx);
// FlameNode: name_idx, cat_idx, total_time, self_time, call_count, first_child, next_sibling, parent (all indices)
// FlameTree: pid, tid, thread_name, root_total_time, first_root, nodes (flat pool)
```

## instance_panel.h / instance_panel.cpp — lists all instances of the selected function; keyboard navigation
```
void render(const TraceModel&, ViewState&);
void on_model_changed();
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
void render_settings();
nlohmann::json save_settings() const;
void load_settings(const nlohmann::json&);
void reset_settings();
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

## key_bindings.h / key_bindings.cpp — configurable keyboard shortcuts with settings UI and persistence
```
enum class Action : int { PanLeft, PanRight, ScrollUp, ScrollDown, ZoomIn, ZoomOut, FitSelection, ClearSelection, GoToTime, OpenFile, Search, OpenSettings, RunQuery, NavParent, NavChild, NavPrevSibling, NavNextSibling, Count };
KeyBindings();
bool is_pressed(Action action) const;
ImGuiKeyChord primary(Action action) const;
ImGuiKeyChord alt(Action action) const;
void reset_defaults();
void render_settings();
nlohmann::json save() const;
void load(const nlohmann::json&);
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
