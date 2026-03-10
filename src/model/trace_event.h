#pragma once
#include <cstdint>

enum class Phase : char {
    DurationBegin = 'B',
    DurationEnd = 'E',
    Complete = 'X',
    Instant = 'i',
    Counter = 'C',
    AsyncBegin = 'b',
    AsyncEnd = 'e',
    AsyncInstant = 'n',
    FlowStart = 's',
    FlowStep = 't',
    FlowEnd = 'f',
    Metadata = 'M',
    ObjectCreated = 'N',
    ObjectSnapshot = 'O',
    ObjectDestroyed = 'D',
    Sample = 'P',
    Mark = 'R',
    Unknown = '?',
};

inline Phase phase_from_char(char c) {
    switch (c) {
        case 'B': return Phase::DurationBegin;
        case 'E': return Phase::DurationEnd;
        case 'X': return Phase::Complete;
        case 'i': case 'I': return Phase::Instant;
        case 'C': return Phase::Counter;
        case 'b': return Phase::AsyncBegin;
        case 'e': return Phase::AsyncEnd;
        case 'n': return Phase::AsyncInstant;
        case 's': return Phase::FlowStart;
        case 't': return Phase::FlowStep;
        case 'f': return Phase::FlowEnd;
        case 'M': return Phase::Metadata;
        case 'N': return Phase::ObjectCreated;
        case 'O': return Phase::ObjectSnapshot;
        case 'D': return Phase::ObjectDestroyed;
        case 'P': return Phase::Sample;
        case 'R': return Phase::Mark;
        default:  return Phase::Unknown;
    }
}

struct TraceEvent {
    uint32_t name_idx = 0;
    uint32_t cat_idx = 0;
    Phase    ph = Phase::Unknown;
    double   ts = 0.0;
    double   dur = 0.0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t id = 0;
    uint32_t args_idx = UINT32_MAX; // index into args storage, UINT32_MAX = no args
    uint8_t  depth = 0;            // nesting depth within thread
    bool     is_end_event = false; // true for matched 'E' events (don't render)

    double end_ts() const { return ts + dur; }
};
