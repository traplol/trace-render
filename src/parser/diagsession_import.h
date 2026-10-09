#pragma once
#include "etl_reader.h"
#include "model/trace_model.h"

bool is_diagsession_container(std::string_view bytes);
bool read_diagsession(std::string_view bytes, TraceModel& model, std::string& error,
                      const ImportProgress& progress = {});
