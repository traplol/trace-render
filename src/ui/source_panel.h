#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

// Extract file and line from an event's args JSON.
// Looks for common field names: file/src_file/fileName + line/src_line/lineNumber.
// Returns true if a file field was found.
bool extract_source_location(const TraceModel& model, const TraceEvent& ev, std::string& file, int& line);

// Remap a source path from a trace to a local filesystem path.
// 1. Normalizes backslashes to forward slashes
// 2. Strips strip_prefix (case-insensitive) from the front if it matches
// 3. Prepends local_base to the remainder
std::string remap_source_path(const std::string& trace_path, const std::string& strip_prefix,
                              const std::string& local_base);

class SourcePanel {
public:
    void render(const TraceModel& model, ViewState& view);

    nlohmann::json save_settings() const;
    void load_settings(const nlohmann::json& j);

private:
    // Path remapping: strip this prefix from trace paths, replace with local base
    char strip_prefix_[512] = {};
    char local_base_[512] = {};

    // Cached state
    int32_t cached_event_idx_ = -1;
    std::string cached_raw_file_;  // raw path from args (before remapping)
    std::string cached_file_;      // resolved local path (after remapping)
    int cached_line_ = -1;
    std::vector<std::string> cached_lines_;
    std::string cached_error_;
    bool need_scroll_ = false;
    bool path_settings_changed_ = false;

    void load_file(const std::string& path);
    void resolve_and_load(const std::string& raw_file);
};
