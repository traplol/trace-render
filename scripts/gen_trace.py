#!/usr/bin/env python3
"""Generate a large Chrome JSON trace file for stress testing."""

import argparse
import json
import random
import sys

FUNCTION_NAMES = [
    "RenderFrame", "UpdatePhysics", "ProcessInput", "DrawScene",
    "LoadTexture", "CompileShader", "BindBuffer", "SwapBuffers",
    "ParseConfig", "SerializeState", "AllocMemory", "FreeMemory",
    "SendPacket", "RecvPacket", "CompressData", "DecompressData",
    "HashTable::Insert", "HashTable::Lookup", "HashTable::Resize",
    "Vector::PushBack", "Vector::Sort", "Vector::Find",
    "FileIO::Read", "FileIO::Write", "FileIO::Flush",
    "DB::Query", "DB::Insert", "DB::Commit", "DB::Rollback",
    "Net::Connect", "Net::Send", "Net::Recv", "Net::Close",
    "GC::Mark", "GC::Sweep", "GC::Compact", "GC::Finalize",
    "Audio::Mix", "Audio::Decode", "Audio::Resample",
    "GPU::Submit", "GPU::Wait", "GPU::Readback",
    "ThreadPool::Dispatch", "ThreadPool::Join", "ThreadPool::Steal",
    "Cache::Get", "Cache::Put", "Cache::Evict", "Cache::Flush",
]

CATEGORIES = ["rendering", "physics", "io", "network", "memory", "audio", "gpu", "general"]

def gen_trace(target_bytes, output):
    num_processes = 4
    threads_per_process = 8

    # Build thread list
    threads = []
    for pid in range(1, num_processes + 1):
        for tid_offset in range(threads_per_process):
            threads.append((pid, pid * 100 + tid_offset))

    # Write metadata events first
    events = []
    for pid in range(1, num_processes + 1):
        events.append({"ph": "M", "name": "process_name", "pid": pid, "tid": 0,
                       "args": {"name": f"Process {pid}"}})
        for tid_offset in range(threads_per_process):
            tid = pid * 100 + tid_offset
            events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": tid,
                           "args": {"name": f"Worker-{tid_offset}"}})

    # Counter for progress
    bytes_written = 0
    event_count = 0

    output.write('{"traceEvents":[\n')

    # Write metadata
    for i, ev in enumerate(events):
        line = json.dumps(ev, separators=(',', ':'))
        if i > 0:
            output.write(',\n')
        output.write(line)
        bytes_written += len(line) + 2

    # Track per-thread timestamp
    thread_ts = {(pid, tid): 0.0 for pid, tid in threads}

    # Generate events until we hit target size
    batch_size = 10000
    while bytes_written < target_bytes:
        for _ in range(batch_size):
            pid, tid = random.choice(threads)
            ts = thread_ts[(pid, tid)]
            name = random.choice(FUNCTION_NAMES)
            cat = random.choice(CATEGORIES)
            dur = random.uniform(1, 5000)  # 1us to 5ms

            # Occasionally generate nested calls
            depth = random.choices([1, 2, 3, 4], weights=[50, 30, 15, 5])[0]

            nested_ts = ts
            nested_events = []
            for d in range(depth):
                n = name if d == 0 else random.choice(FUNCTION_NAMES)
                c = cat if d == 0 else random.choice(CATEGORIES)
                nd = dur / (d + 1) * random.uniform(0.5, 0.95)
                ev = {"ph": "X", "name": n, "cat": c, "ts": round(nested_ts, 1),
                      "dur": round(nd, 1), "pid": pid, "tid": tid}
                if random.random() < 0.1:
                    ev["args"] = {"count": random.randint(1, 1000),
                                  "size": random.randint(64, 65536)}
                nested_events.append(ev)
                nested_ts += random.uniform(0, nd * 0.1)
                dur = nd

            for ev in nested_events:
                line = json.dumps(ev, separators=(',', ':'))
                output.write(',\n')
                output.write(line)
                bytes_written += len(line) + 2
                event_count += 1

            thread_ts[(pid, tid)] = ts + dur + random.uniform(0, 100)

        # Progress
        pct = min(100, bytes_written * 100 // target_bytes)
        print(f"\r  {bytes_written / (1024*1024):.0f} MB / {target_bytes / (1024*1024):.0f} MB ({pct}%) - {event_count} events", end='', file=sys.stderr)

    # Add some counter events
    for pid in range(1, num_processes + 1):
        ts = 0.0
        for _ in range(1000):
            ev = {"ph": "C", "name": "Memory", "pid": pid, "tid": 0,
                  "ts": round(ts, 1), "args": {"heap_mb": round(random.uniform(100, 500), 1)}}
            line = json.dumps(ev, separators=(',', ':'))
            output.write(',\n')
            output.write(line)
            ts += random.uniform(1000, 50000)

    # Add some flow events
    flow_id = 1
    for _ in range(500):
        pid1, tid1 = random.choice(threads)
        pid2, tid2 = random.choice(threads)
        ts1 = random.uniform(0, 1000000)
        ts2 = ts1 + random.uniform(100, 10000)
        for ev in [
            {"ph": "s", "name": "Flow", "cat": "flow", "id": flow_id, "ts": round(ts1, 1), "pid": pid1, "tid": tid1},
            {"ph": "f", "name": "Flow", "cat": "flow", "id": flow_id, "ts": round(ts2, 1), "pid": pid2, "tid": tid2},
        ]:
            line = json.dumps(ev, separators=(',', ':'))
            output.write(',\n')
            output.write(line)
        flow_id += 1

    output.write('\n]}')
    print(f"\nDone: {bytes_written / (1024*1024):.0f} MB, {event_count} events", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Generate a large Chrome trace file")
    parser.add_argument("-s", "--size", type=int, default=1024,
                        help="Target size in MB (default: 1024)")
    parser.add_argument("-o", "--output", default="test_trace.json",
                        help="Output file (default: test_trace.json)")
    args = parser.parse_args()

    target = args.size * 1024 * 1024
    print(f"Generating {args.size} MB trace -> {args.output}", file=sys.stderr)

    with open(args.output, 'w') as f:
        gen_trace(target, f)


if __name__ == "__main__":
    main()
