#pragma once
#include "model/trace_model.h"
#include <string>
#include <string_view>

inline constexpr std::string_view PROFILE_MAGIC = "TRPROFILE\n";
inline constexpr uint32_t PROFILE_VERSION = 1;

// Parse only the versioned profile format. TraceParser dispatches Chrome JSON separately.
bool read_profile(std::string_view data, TraceModel& model, std::string& error);
bool serialize_profile(const TraceModel& model, std::string& data, std::string& error);
bool write_profile(const std::string& filepath, const TraceModel& model, std::string& error);
