#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// IDs are opaque capture-local identities, never display names or reused OS addresses.
struct ProfileProcess {
    std::string id;
    uint32_t pid = 0;
    std::optional<double> start_ts;
    std::optional<double> end_ts;
};

struct ProfileHeap {
    std::string id;
    std::string process_id;
    std::optional<double> start_ts;
    std::optional<double> end_ts;
};

struct ProfileModule {
    std::string id;
    std::string process_id;
    std::string name;
    std::string path;
    std::string build_id;  // e.g. the matching PDB GUID and age
    uint64_t load_address = 0;
    uint64_t size_bytes = 0;
    std::optional<double> load_ts;
    std::optional<double> unload_ts;
    std::string pdb_path;  // recorded RSDS path; only its basename is searched in supplied directories
};

enum class AllocationKind : uint8_t { Native, Managed };
enum class AllocationEnd : uint8_t { Unknown, Freed, LiveAtCaptureEnd };

struct AllocationLifetime {
    std::string id;  // unique allocation generation, including reuse of the same address
    std::string process_id;
    std::string heap_id;
    std::optional<uint64_t> address;
    uint64_t size_bytes = 0;
    std::string stack_frame_id;
    std::string type_id;
    AllocationKind kind = AllocationKind::Native;
    std::optional<double> allocated_ts;  // null means the start was not observed
    std::optional<double> freed_ts;      // null never implies live or freed
    AllocationEnd end_state = AllocationEnd::Unknown;
};

struct ManagedTypeSummary {
    std::string type_id;
    std::string name;
    uint64_t object_count = 0;
    uint64_t size_bytes = 0;
    double count_multiplier = 1;  // producer's sampling weight; counts above remain recorded counts
};

struct ManagedSnapshot {
    std::string id;
    std::string process_id;
    double ts = 0;
    std::optional<uint64_t> live_bytes;  // recorded graph bytes; no sampling multiplier is applied
    std::optional<uint64_t> object_count;
    std::vector<ManagedTypeSummary> types;
    bool sampled = false;
    bool incomplete = false;
    double average_count_multiplier = 1;
    double average_size_multiplier = 1;
    std::vector<std::string> warnings;
};

struct ManagedSurvivalObservation {
    std::string allocation_id;
    double ts = 0;  // GC observation time, not an inferred exact free time
    bool survived = true;
    std::optional<uint64_t> address;  // observed location after a possible GC move
};

struct ProfileCapabilities {
    bool native_cpu_samples = false;
    bool managed_cpu_samples = false;
    bool native_allocation_history = false;
    bool managed_allocation_history = false;
    bool managed_heap_snapshots = false;
    bool managed_survival = false;
};

struct ProfileQuality {
    bool incomplete_capture = false;
    bool sampled_cpu = false;
    bool sampled_allocations = false;
    bool unresolved_symbols = false;
    std::optional<uint64_t> lost_events;
    std::vector<std::string> warnings;
    bool allocation_history_gaps = false;  // missing relevant events inside the recorded history
};

struct ProfileData {
    std::string source_format;
    std::string source_name;
    std::string converter;
    std::optional<double> capture_start_ts;
    std::optional<double> capture_end_ts;
    ProfileCapabilities capabilities;
    ProfileQuality quality;
    std::vector<ProfileProcess> processes;
    std::vector<ProfileHeap> heaps;
    std::vector<ProfileModule> modules;
    std::vector<AllocationLifetime> allocations;
    std::vector<ManagedSnapshot> managed_snapshots;
    std::vector<ManagedSurvivalObservation> managed_survival;
};
