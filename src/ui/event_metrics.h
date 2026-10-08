#pragma once
#include "model/trace_event.h"
#include "format_time.h"
#include <cstdio>

// Shared table/tooltip value. Never format a sample as a measured duration.
inline void format_event_metric(const TraceEvent& ev, char* buf, size_t size) {
    if (ev.kind == EventKind::Measured) {
        format_time(ev.dur, buf, size);
    } else {
        double cpu = ev.kind == EventKind::Sample ? ev.sample_cpu_time : ev.dur;
        if (cpu < 0)
            snprintf(buf, size, "CPU time unavailable");
        else {
            char time[64];
            format_time(cpu, time, sizeof(time));
            snprintf(buf, size, "Est. CPU %s", time);
        }
    }
}
