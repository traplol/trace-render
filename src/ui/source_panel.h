#pragma once
#include "model/trace_model.h"
#include "ui/view_state.h"
#include <string>
#include <vector>

// Extract file and line from an event's args JSON.
// Looks for common field names: file/src_file/fileName + line/src_line/lineNumber.
// Returns true if a file field was found.
bool extract_source_location(const TraceModel& model, const TraceEvent& ev, std::string& file, int& line);

class SourcePanel {
public:
    void render(const TraceModel& model, ViewState& view);

private:
    // Source root prefix to prepend to relative paths
    char source_root_[512] = {};

    // Cached state
    int32_t cached_event_idx_ = -1;
    std::string cached_file_;
    int cached_line_ = -1;
    std::vector<std::string> cached_lines_;
    std::string cached_error_;
    bool need_scroll_ = false;

    void load_file(const std::string& path);
};
