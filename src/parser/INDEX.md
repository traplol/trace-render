# src/parser/
Chrome JSON, portable desktop diagsession import, and versioned TraceRender profile import/export.

## trace_parser.h / trace_parser.cpp — dispatches diagsessions and saved profiles and streams Chrome events, stack definitions, and sampling weights into `TraceModel`
```
bool parse(const std::string& filepath, TraceModel& model);
bool parse_buffer(const char* data, size_t size, TraceModel& model);
const std::function<void(const char*, float)>& on_progress() const;
void set_on_progress(std::function<void(const char*, float)> cb);
const std::string& error_message() const;
bool time_unit_ns() const;
void set_time_unit_ns(bool ns);
void set_native_symbols(NativeSymbolResolver* symbols);
```

## profile_io.h / profile_io.cpp — reads and writes version 1 profiles with validated identities, CPU observations, allocation lifetimes, managed snapshot quality and sampling weights, optional recorded PDB paths, and allocation-gap metadata
```
bool read_profile(std::string_view data, TraceModel& model, std::string& error);
bool serialize_profile(const TraceModel& model, std::string& data, std::string& error);
bool write_profile(const std::string& filepath, const TraceModel& model, std::string& error);
```

## native_heap.h / native_heap.cpp — replays decoded native heap events into allocation and heap generations while preserving allocation origins and reporting incomplete coverage
```
void build_native_allocations(const std::vector<NativeHeapEvent>& events, ProfileData& profile, bool complete_event_stream);
```

## diagsession_import.h / diagsession_import.cpp — converts metadata-referenced ETL and managed snapshot resources into mixed managed/native CPU samples, native allocations, snapshot summaries, and capture-local identities
```
bool is_diagsession_container(std::string_view bytes);
bool read_diagsession(std::string_view bytes, TraceModel& model, std::string& error, const ImportProgress& progress = {}, NativeSymbolResolver* symbols = nullptr);
```

## managed_methods.h / managed_methods.cpp — resolves recorded CLR method ranges by process and JIT generation while preserving ambiguous or incomplete metadata as unresolved frames
```
bool ManagedMethods::consume(const EtlRecord& record);
void ManagedMethods::build(const std::function<std::string(uint32_t, uint64_t)>& process_at);
const ManagedMethod* ManagedMethods::find(const std::string& process_id, uint64_t address, uint64_t qpc) const;
const std::vector<std::string>& ManagedMethods::warnings() const;
```

## managed_snapshot.h / managed_snapshot.cpp — reads bounded GCHeapDump graphs and manifest-referenced type summaries while preserving snapshot sampling and partial-data limits
```
bool read_gcdump(std::string_view bytes, ManagedSnapshot& snapshot, std::string& error, const ImportProgress& progress = {});
bool read_managed_snapshots(const DiagsessionContainer& container, ProfileData& profile, std::string& error, const ImportProgress& progress = {});
```

## etl_reader.h / etl_reader.cpp — traverses checked ETL buffers and records, preserves raw QPC and provider payloads, and decompresses supported XPRESS buffers
```
explicit EtlBytes(std::string_view bytes);
uint8_t EtlBytes::u8(size_t offset) const;
uint16_t EtlBytes::u16(size_t offset) const;
uint32_t EtlBytes::u32(size_t offset) const;
uint64_t EtlBytes::u64(size_t offset) const;
uint64_t EtlBytes::pointer(size_t offset, uint32_t width) const;
std::string_view EtlBytes::slice(size_t offset, size_t size) const;
std::string EtlBytes::utf8(size_t offset) const;
std::string EtlBytes::utf16(size_t offset) const;
std::string EtlBytes::guid(size_t offset) const;
bool read_etl(std::string_view bytes, EtlFileInfo& info, const EtlRecordCallback& consume, std::string& error, const ImportProgress& progress = {});
```

## diagsession_container.h / diagsession_container.cpp — reads metadata-referenced ZIP and CFB resources lazily in memory, including DiagnosticsHub compression wrappers and CRC validation
```
DiagsessionContainer();
~DiagsessionContainer();
bool DiagsessionContainer::open(std::string_view bytes, std::string& error);
const std::vector<DiagsessionResource>& DiagsessionContainer::resources() const;
const std::string& DiagsessionContainer::format() const;
const std::string& DiagsessionContainer::producer_version() const;
bool DiagsessionContainer::read_resource(size_t index, std::vector<uint8_t>& output, std::string& error, std::function<bool()> cancelled = {}) const;
```

## xpress_huffman.h / xpress_huffman.cpp — decompresses bounded MS-XCA XPRESS-Huffman chunks without platform APIs
```
bool decompress_xpress_huffman(std::string_view input, size_t expected_size, std::vector<uint8_t>& output, std::string& error);
```
