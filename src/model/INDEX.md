# src/model/

Core data layer. Defines the in-memory representation of a loaded trace (events, processes, threads, counters, flow groups), the spatial block index for fast range queries, the SQLite-backed query database, and the color palette.

## Files

### `trace_event.h`
Defines the `Phase` enum and the `TraceEvent` struct — the fundamental unit of trace data. Every event in a loaded trace is stored as a `TraceEvent`.

```cpp
enum class Phase : char {
    DurationBegin, DurationEnd, Complete, Instant,
    Counter, AsyncBegin, AsyncEnd, AsyncInstant,
    FlowStart, FlowStep, FlowEnd, Metadata,
    ObjectCreated, ObjectSnapshot, ObjectDestroyed,
    Sample, Mark, Unknown
};

Phase phase_from_char(char c);

struct TraceEvent {
    uint32_t name_idx;   // index into TraceModel::strings_
    uint32_t cat_idx;
    Phase ph;
    double ts;           // timestamp in microseconds
    double dur;
    uint32_t pid;
    uint32_t tid;
    uint64_t id;         // async/flow correlation id
    uint32_t args_idx;   // UINT32_MAX = no args
    uint8_t depth;       // nesting depth within thread
    bool is_end_event;   // true for matched 'E' events; not rendered
    int32_t parent_idx;  // -1 = root
    double self_time;    // dur minus immediate children

    double end_ts() const;
};
```

---

### `trace_model.h` / `trace_model.cpp`
The central data model holding the entire loaded trace. Contains a flat event array, an interned string pool, process/thread hierarchy, counter series, and flow groups. `build_index()` sorts events per-thread, computes depths/parents/self-times, and builds spatial `BlockIndex`es.

```cpp
struct ThreadInfo {
    uint32_t tid;
    std::string name;
    std::vector<uint32_t> event_indices;  // sorted by ts
    int32_t sort_index;
    uint8_t max_depth;
    BlockIndex block_index;

    const ThreadInfo* find_thread(uint32_t tid) const;
    ThreadInfo* find_thread(uint32_t tid);
    ThreadInfo& get_or_create_thread(uint32_t tid);
};

struct ProcessInfo {
    uint32_t pid;
    std::string name;
    std::vector<ThreadInfo> threads;
    int32_t sort_index;

    const ThreadInfo* find_thread(uint32_t tid) const;
    ThreadInfo* find_thread(uint32_t tid);
    ThreadInfo& get_or_create_thread(uint32_t tid);
};

struct CounterSeries {
    uint32_t pid;
    std::string name;
    std::vector<std::pair<double, double>> points;  // (timestamp, value)
    double min_val;
    double max_val;
};

class TraceModel {
    // Storage
    std::vector<TraceEvent> events_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> string_map_;
    std::vector<std::string> args_;
    std::vector<ProcessInfo> processes_;
    std::vector<CounterSeries> counter_series_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> flow_groups_;
    double min_ts_;
    double max_ts_;
    std::vector<uint32_t> categories_;  // sorted alphabetically

    // String pool
    uint32_t intern_string(const std::string& s);
    const std::string& get_string(uint32_t idx) const;

    // Process/thread lookup
    const ProcessInfo* find_process(uint32_t pid) const;
    ProcessInfo* find_process(uint32_t pid);
    const ThreadInfo* find_thread(uint32_t pid, uint32_t tid) const;
    ThreadInfo* find_thread(uint32_t pid, uint32_t tid);
    ProcessInfo& get_or_create_process(uint32_t pid);

    // Post-parse indexing
    void build_index(std::function<void(float)> on_progress = nullptr);

    // Event relationship queries
    int32_t find_parent_event(uint32_t event_idx) const;
    std::vector<uint32_t> build_call_stack(uint32_t event_idx) const;
    double compute_self_time(uint32_t event_idx) const;

    // Visible range query (delegates to BlockIndex)
    void query_visible(const ThreadInfo& thread, double start_ts, double end_ts,
                       std::vector<uint32_t>& out) const;

    void clear();
};
```

---

### `block_index.h`
Spatial index over a sorted event array. Groups events into 256-element blocks, each storing the min timestamp and propagated max end-timestamp, enabling a binary-search skip of non-overlapping blocks during range queries.

```cpp
struct BlockIndex {
    static constexpr size_t BLOCK_SIZE = 256;

    struct Block {
        double min_ts;
        double max_end_ts;        // propagated max for binary search
        double local_max_end_ts;  // max of this block only
        uint32_t start_idx;
        uint32_t count;
        uint32_t depth_mask;      // bitmask of depths present
    };

    std::vector<Block> blocks;

    void build(const std::vector<uint32_t>& event_indices,
               const std::vector<TraceEvent>& events);
    void query(double start_ts, double end_ts,
               const std::vector<uint32_t>& event_indices,
               const std::vector<TraceEvent>& events,
               std::vector<uint32_t>& out) const;
    size_t find_first_block(double start_ts) const;
};
```

---

### `query_db.h` / `query_db.cpp`
SQLite-backed database populated from a `TraceModel`. Supports synchronous and asynchronous query execution with cancellation, row-count progress, and background index creation.

```cpp
class QueryDb {
    QueryDb();
    ~QueryDb();

    struct QueryResult {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        std::string error;
        bool ok;
    };

    void load(const TraceModel& model,
              std::function<void(float)> on_progress = nullptr);

    // Synchronous
    QueryResult execute(const std::string& sql);

    // Asynchronous
    void execute_async(const std::string& sql);
    bool is_query_running() const;
    bool is_query_done() const;
    void cancel_query();
    QueryResult take_result();
    int query_rows_so_far() const;
    uint64_t query_steps() const;

    bool is_loaded() const;

    // Background index creation
    void create_indexes_async();
    bool is_indexing() const;
    float indexing_progress() const;
};
```

---

### `color_palette.h`
48-color palette with hash-based event coloring. Picks fill colors from the palette using category and name indices, and derives a darker border color and a contrasting text color from the fill.

```cpp
struct ColorPalette {
    static constexpr ImU32 COLORS[48];
    static constexpr size_t NUM_COLORS;

    static ImU32 color_for_event(uint32_t cat_idx, uint32_t name_idx);
    static ImU32 color_for_category(uint32_t cat_idx);
    static ImU32 border_color(ImU32 fill);
    static ImU32 text_color(ImU32 bg);
};
```
