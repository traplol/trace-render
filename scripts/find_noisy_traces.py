#!/usr/bin/env python3
"""Find functions called more than a given rate in a Chrome trace JSON file.

Usage:
    python3 scripts/find_noisy_traces.py <trace.json> [--threshold 200]

Streams the file so it can handle multi-GB traces without loading into memory.
"""

import argparse
import ijson
from collections import Counter


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("trace", help="Path to Chrome trace JSON file")
    p.add_argument("--threshold", type=float, default=200, help="Min calls/second to report (default: 200)")
    args = p.parse_args()

    counts = Counter()
    min_ts = float("inf")
    max_ts = 0

    with open(args.trace, "rb") as f:
        for event in ijson.items(f, "traceEvents.item"):
            name = event.get("name", "")
            ts = event.get("ts", 0)
            if ts:
                min_ts = min(min_ts, ts)
                max_ts = max(max_ts, ts)
            if name:
                counts[name] += 1

    duration_s = (max_ts - min_ts) / 1_000_000
    total = sum(counts.values())
    min_count = args.threshold * duration_s

    print(f"Trace duration: {duration_s:.1f}s")
    print(f"Total events: {total}")
    print(f"Threshold: {args.threshold:.0f}/s (>{int(min_count)} total)")
    print()

    found = 0
    for name, count in counts.most_common():
        rate = count / duration_s
        if rate < args.threshold:
            break
        found += 1
        print(f"  {rate:8.0f}/s  {count:8d}x  {name}")

    if not found:
        print("  (none)")


if __name__ == "__main__":
    main()
