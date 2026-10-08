#pragma once
#include <cstdint>
#include <optional>

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
        case 'B':
            return Phase::DurationBegin;
        case 'E':
            return Phase::DurationEnd;
        case 'X':
            return Phase::Complete;
        case 'i':
        case 'I':
            return Phase::Instant;
        case 'C':
            return Phase::Counter;
        case 'b':
            return Phase::AsyncBegin;
        case 'e':
            return Phase::AsyncEnd;
        case 'n':
            return Phase::AsyncInstant;
        case 's':
            return Phase::FlowStart;
        case 't':
            return Phase::FlowStep;
        case 'f':
            return Phase::FlowEnd;
        case 'M':
            return Phase::Metadata;
        case 'N':
            return Phase::ObjectCreated;
        case 'O':
            return Phase::ObjectSnapshot;
        case 'D':
            return Phase::ObjectDestroyed;
        case 'P':
            return Phase::Sample;
        case 'R':
            return Phase::Mark;
        default:
            return Phase::Unknown;
    }
}

enum class EventKind : uint8_t { Measured, Sample, SampledSpan };

inline const char* event_kind_name(EventKind kind) {
    switch (kind) {
        case EventKind::Sample:
            return "CPU sample";
        case EventKind::SampledSpan:
            return "Sampled span";
        default:
            return "Measured";
    }
}

struct StackFrame {
    uint32_t id_idx = UINT32_MAX;
    uint32_t name_idx = 0;
    uint32_t cat_idx = 0;
    uint32_t parent_id = UINT32_MAX;
    int32_t parent_idx = -1;
    bool valid = true;
    uint32_t module_id = UINT32_MAX;  // interned ProfileModule::id
    uint32_t symbol_id = UINT32_MAX;  // optional function identity, distinct from display name
    std::optional<uint64_t> address;
    uint32_t source_file = 0;
    uint32_t source_line = 0;  // zero means unavailable
    bool symbol_resolved = false;
};

struct TraceEvent {
    uint32_t name_idx = 0;
    uint32_t cat_idx = 0;
    Phase ph = Phase::Unknown;
    double ts = 0.0;
    double dur = 0.0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t id = 0;
    uint32_t process_instance_id = UINT32_MAX;  // interned ProfileProcess::id
    uint32_t args_idx = UINT32_MAX;             // index into args storage, UINT32_MAX = no args
    uint8_t depth = 0;                          // nesting depth within thread
    bool is_end_event = false;                  // true for matched 'E' events (don't render)
    int32_t parent_idx = -1;                    // index of parent event (-1 if root or no parent)
    double self_time = 0.0;                     // measured/estimated span time minus immediate children
    EventKind kind = EventKind::Measured;
    uint32_t stack_frame_id = UINT32_MAX;  // interned sf, preserved even if unresolved
    int32_t stack_frame_idx = -1;
    double sample_weight = -1.0;      // raw weight; negative means unavailable
    uint32_t sample_weight_unit = 0;  // interned explicit weightUnit, if supplied
    double sample_cpu_time = -1.0;    // estimated microseconds; negative means unavailable

    double end_ts() const { return ts + dur; }
};
