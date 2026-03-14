# src/ui/

All ImGui-based UI components. Each panel is a self-contained class with a `render()` method. Shared state flows through `ViewState`. Utility headers provide time formatting, string search, and sorting helpers used across panels.

## Files

### `view_state.h`
Shared viewport and interaction state passed (by reference) to every render call. Tracks the visible time range, selected event, range selection, filter sets, search results, layout constants, and time-conversion helpers.

```cpp
struct ViewState {
    double view_start_ts;
    double view_end_ts;

    int32_t selected_event_idx;
    int32_t pending_scroll_event_idx;

    bool has_range_selection;
    bool range_selecting;
    double range_start_ts;
    double range_end_ts;

    std::unordered_set<uint32_t> hidden_pids;
    std::unordered_set<uint32_t> hidden_tids;
    std::unordered_set<uint32_t> hidden_cats;

    std::string search_query;
    std::vector<uint32_t> search_results;
    int32_t search_current;

    float track_height;
    float track_padding;
    float counter_track_height;
    float label_width;
    float ruler_height;
    float proc_header_height;
    float scrollbar_scale;

    float sel_border_color[4];
    bool show_flows;
    bool time_unit_ns;

    void set_range_selection(double start, double end);
    void clear_range_selection();
    ImU32 sel_border_color_u32() const;
    float time_to_x(double ts, float timeline_left, float timeline_width) const;
    double x_to_time(float x, float timeline_left, float timeline_width) const;
    void zoom_to_fit(double min_ts, double max_ts);
    void navigate_to_event(int32_t ev_idx, const TraceEvent& ev,
                           double pad_factor = 0.5, double min_pad_us = 100.0);
};
```

---

### `timeline_view.h` / `timeline_view.cpp`
Main timeline rendering. Draws the time ruler, process/thread track labels, event boxes (with sub-pixel merging), instant events, counter tracks, flow arrows, and handles mouse interaction (click-to-select, scroll, zoom, ruler drag for range selection).

```cpp
class TimelineView {
    void render(const TraceModel& model, ViewState& view);

    DiagStats diag_stats;  // updated each frame, read by DiagnosticsPanel
};
```

---

### `detail_panel.h` / `detail_panel.cpp`
Shows details for the selected event: name, category, timestamps, duration, self-time, args, call stack, and a children table (flat or aggregated by name). Also shows range-selection summary when no event is selected.

```cpp
class DetailPanel {
    void render(const TraceModel& model, ViewState& view);
};
```

---

### `search_panel.h` / `search_panel.cpp`
Text search over event names. Populates `ViewState::search_results` and allows navigating between matches.

```cpp
class SearchPanel {
    void render(const TraceModel& model, ViewState& view);
};
```

---

### `filter_panel.h` / `filter_panel.cpp`
Checkboxes to hide/show individual processes, threads, and categories. Writes to `ViewState::hidden_pids/tids/cats`.

```cpp
class FilterPanel {
    void render(const TraceModel& model, ViewState& view);
};
```

---

### `counter_track.h` / `counter_track.cpp`
Renders counter series (time-series data) as filled step-function graphs inside the timeline. Handles sub-pixel point merging and hover hit-testing.

```cpp
bool counter_lookup_value(const CounterSeries& series, double time,
                          double& out_timestamp, double& out_value);

struct MergedCounterSegment {
    float x;
    double min_val;
    double max_val;
    double last_val;
    int point_count;
};

std::vector<MergedCounterSegment> merge_counter_points(
    const std::vector<std::pair<double, double>>& points,
    double view_start, double view_end, float track_x, float track_w);

class CounterTrackRenderer {
    float render(ImDrawList* dl, ImVec2 area_min, float y_offset, float width,
                 const TraceModel& model, uint32_t pid, const ViewState& view);
    void render_series(ImDrawList* dl, ImVec2 track_min, ImVec2 track_max,
                       const CounterSeries& series, const ViewState& view, ImU32 color);
    bool hit_test(float mouse_x, float mouse_y, const ViewState& view,
                  CounterHitResult& result) const;
};
```

---

### `stats_panel.h` / `stats_panel.cpp`
SQL query editor and result table. Includes a visual query builder that generates SELECT statements and a tabbed interface for saving multiple queries. Tab state is serialized to/from JSON for settings persistence.

```cpp
struct QueryBuilderState {
    int table_idx;

    struct SelectCol { int col_idx; int agg_idx; char alias[64]; };
    std::vector<SelectCol> select_cols;

    struct WhereClause { int col_idx; int op_idx; char value[256]; int logic_idx; };
    std::vector<WhereClause> where_clauses;

    std::vector<int> group_cols;

    struct HavingClause { int agg_idx; int col_idx; int op_idx; char value[128]; };
    std::vector<HavingClause> having_clauses;

    struct OrderCol { int col_idx; bool descending; };
    std::vector<OrderCol> order_cols;

    bool use_limit;
    int limit_value;

    void reset();
    std::string build_sql(const char* const* columns, int num_columns) const;
};

class StatsPanel {
    void render(const TraceModel& model, QueryDb& db, ViewState& view);
    nlohmann::json save_tabs() const;
    void load_tabs(const nlohmann::json& j);
};
```

---

### `instance_panel.h` / `instance_panel.cpp`
Lists all instances of the currently selected function name, sorted by timestamp. Allows navigating between instances with keyboard shortcuts.

```cpp
class InstancePanel {
    void render(const TraceModel& model, ViewState& view);
};
```

---

### `diagnostics_panel.h` / `diagnostics_panel.cpp`
Developer diagnostics panel. Shows FPS, frame time, memory usage sparklines, and per-frame render stats (visible/drawn/merged slices, label count, etc.) supplied by `TimelineView`.

```cpp
struct DiagStats {
    int visible_slices;
    int drawn_slices;
    int merged_slices;
    int merge_runs;
    int labels_drawn;
    int tracks_visible;
    int instant_events;
};

class DiagnosticsPanel {
    void render(const TraceModel& model, const ViewState& view);
    float current_rss_mb() const;
    DiagStats stats;
};
```

---

### `source_panel.h` / `source_panel.cpp`
Displays source code for the selected event by reading the file and line number from the event's args. Supports configurable path prefix stripping and local base remapping for traces captured on a different machine.

```cpp
bool extract_source_location(const TraceModel& model, const TraceEvent& ev,
                             std::string& file, int& line);

std::string remap_source_path(const std::string& trace_path,
                              const std::string& strip_prefix,
                              const std::string& local_base);

class SourcePanel {
    void render(const TraceModel& model, ViewState& view);
    nlohmann::json save_settings() const;
    void load_settings(const nlohmann::json& j);
};
```

---

### `flow_renderer.h` / `flow_renderer.cpp`
Draws bezier-curve arrows connecting flow/async events across tracks. Receives track Y positions from `TimelineView` and uses `ViewState` for time-to-pixel mapping.

```cpp
class FlowRenderer {
    struct TrackPos { float y_start; float height; };

    void set_track_positions(const std::unordered_map<uint64_t, TrackPos>& positions);
    void render(ImDrawList* dl, const TraceModel& model, const ViewState& view,
                ImVec2 area_min, ImVec2 area_max, float label_width);
    static uint64_t make_key(uint32_t pid, uint32_t tid);
};
```

---

### `toolbar.h` / `toolbar.cpp`
Top toolbar. Renders the open-file button, zoom controls, time unit toggle, flow arrow toggle, memory readout, and settings button.

```cpp
class Toolbar {
    void render(const TraceModel& model, ViewState& view, float rss_mb);
    void set_window(SDL_Window* window);
    bool settings_requested() const;
    void clear_settings_request();
};
```

---

### `range_stats.h` / `range_stats.cpp`
Computes per-name event statistics (count, total/min/max/avg duration, longest instance) for all events overlapping a given time range. Used by `DetailPanel` when a range is selected.

```cpp
struct RangeEventSummary {
    uint32_t name_idx;
    uint32_t count;
    double total_dur;
    double min_dur;
    double max_dur;
    uint32_t longest_idx;

    double avg_dur() const;
};

struct RangeStats {
    double range_duration;
    uint32_t total_events;
    std::vector<RangeEventSummary> summaries;  // sorted by total_dur descending
};

RangeStats compute_range_stats(const TraceModel& model,
                               double start_ts, double end_ts);
```

---

### `format_time.h`
Inline time formatting utilities. Always use these instead of writing inline formatting.

```cpp
void format_time(double us, char* buf, size_t buf_size);
void format_ruler_time(double us, double tick_interval, char* buf, size_t buf_size);
```

---

### `string_utils.h`
Case-insensitive substring search.

```cpp
bool contains_case_insensitive(const std::string& haystack, const std::string& needle);
```

---

### `sort_utils.h`
Three-way comparison helper for ImGui table sort callbacks.

```cpp
namespace sort_utils {
    template <typename T>
    int three_way_cmp(const T& a, const T& b);  // returns -1, 0, or 1
}
```
