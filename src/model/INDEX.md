# src/model/
Core data layer: in-memory trace representation, spatial block index, SQLite query DB, color palette.

## trace_event.h — `Phase` enum and `TraceEvent` struct (fundamental event unit)
```
Phase phase_from_char(char c);
double TraceEvent::end_ts() const;
```

## trace_model.h / trace_model.cpp — central model: flat event array, string pool, process/thread hierarchy, counters, flows, name-to-events index
```
// ProcessInfo
const ThreadInfo* find_thread(uint32_t tid) const;
ThreadInfo* find_thread(uint32_t tid);
ThreadInfo& get_or_create_thread(uint32_t tid);
// TraceModel — const accessors
const std::vector<TraceEvent>& events() const;
const std::vector<std::string>& strings() const;
const std::unordered_map<std::string, uint32_t>& string_map() const;
const std::vector<std::string>& args() const;
const std::vector<ProcessInfo>& processes() const;
const std::vector<CounterSeries>& counter_series() const;
const std::unordered_map<uint64_t, std::vector<uint32_t>>& flow_groups() const;
double min_ts() const;
double max_ts() const;
const std::vector<uint32_t>& categories() const;
const std::unordered_map<uint32_t, std::vector<uint32_t>>& name_to_events() const;
// TraceModel — mutation methods
uint32_t add_event(const TraceEvent& ev);
uint32_t add_args(std::string args_json);
void add_flow_event(uint64_t id, uint32_t event_idx);
CounterSeries& find_or_create_counter_series(uint32_t pid, const std::string& name);
uint32_t intern_string(const std::string&);
const std::string& get_string(uint32_t idx) const;
const ProcessInfo* find_process(uint32_t pid) const;
ProcessInfo* find_process(uint32_t pid);
const ThreadInfo* find_thread(uint32_t pid, uint32_t tid) const;
ThreadInfo* find_thread(uint32_t pid, uint32_t tid);
ProcessInfo& get_or_create_process(uint32_t pid);
void build_index(std::function<void(float)> on_progress = nullptr);
int32_t find_parent_event(uint32_t event_idx) const;
std::vector<uint32_t> build_call_stack(uint32_t event_idx) const;
double compute_self_time(uint32_t event_idx) const;
int32_t find_first_child(uint32_t event_idx) const;
int32_t find_prev_sibling(uint32_t event_idx) const;
int32_t find_next_sibling(uint32_t event_idx) const;
void query_visible(const ThreadInfo&, double start_ts, double end_ts, std::vector<uint32_t>& out) const;
void clear();
```

## block_index.h — spatial index (256-event blocks) for binary-search range queries
```
const std::vector<Block>& blocks() const;
void build(const std::vector<uint32_t>& event_indices, const std::vector<TraceEvent>& events);
void query(double start_ts, double end_ts, const std::vector<uint32_t>& event_indices,
           const std::vector<TraceEvent>& events, std::vector<uint32_t>& out) const;
size_t find_first_block(double start_ts) const;
```

## query_db.h / query_db.cpp — SQLite DB populated from `TraceModel`; sync + async query execution
```
void load(const TraceModel&, std::function<void(float)> on_progress = nullptr);
QueryResult execute(const std::string& sql);
void execute_async(const std::string& sql);
void cancel_query();
QueryResult take_result();
bool is_query_running() const;
bool is_query_done() const;
bool is_loaded() const;
int query_rows_so_far() const;
uint64_t query_steps() const;
void create_indexes_async();
bool is_indexing() const;
float indexing_progress() const;
// QueryResult fields: columns, rows, error, ok
```

## color_palette.h — 48-color palette; hash-based event coloring, border, and text contrast
```
static ImU32 color_for_event(uint32_t cat_idx, uint32_t name_idx);
static ImU32 color_for_category(uint32_t cat_idx);
static ImU32 border_color(ImU32 fill);
static ImU32 text_color(ImU32 bg);
```
