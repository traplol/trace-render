#pragma once
#include "etl_reader.h"
#include "model/profile_data.h"
#include <string>
#include <string_view>

class DiagsessionContainer;

// Reads the bounded GCHeapDump v10 / MemoryGraph v1, minReader0 layout used by
// the validated VS and PerfView snapshots. Counts are recorded, never weighted.
// The caller supplies capture-local id, process_id and timestamp after reading.
bool read_gcdump(std::string_view bytes, ManagedSnapshot& snapshot, std::string& error,
                 const ImportProgress& progress = {});

// Adds supported snapshots referenced by a MemoryProfiler.Manifest. Unsupported
// individual snapshots become import warnings so other views remain available.
bool read_managed_snapshots(const DiagsessionContainer& container, ProfileData& profile, std::string& error,
                            const ImportProgress& progress = {});
