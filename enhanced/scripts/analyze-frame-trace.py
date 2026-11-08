#!/usr/bin/env python3
"""Summarize an OPENMW_FRAME_TRACE capture without starting OpenMW."""

import argparse
from bisect import bisect_left, bisect_right
from collections import defaultdict
import json
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def overlap_index(events):
    intervals = sorted((event["ts"], event["ts"] + event["dur"]) for event in events)
    return sorted(start for start, _ in intervals), sorted(end for _, end in intervals)


def overlaps(index, event):
    starts, ends = index
    return bisect_left(starts, event["ts"] + event["dur"]) > bisect_right(ends, event["ts"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--skip-seconds", type=float, default=0,
                        help="ignore this many seconds after the first recorded main loop")
    parser.add_argument("--spike-ms", type=float, default=33.333)
    args = parser.parse_args()
    if args.skip_seconds < 0 or args.spike_ms <= 0:
        parser.error("skip-seconds must be nonnegative and spike-ms must be positive")
    try:
        with args.trace.open(encoding="utf-8") as stream:
            trace = json.load(stream)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Cannot read trace: {error}\nQuit the game normally to finalize its JSON.\n")
    events = [event for event in trace.get("traceEvents", []) if event.get("ph") == "X"]
    loops = [event for event in events if event["name"] == "Main loop"]
    if not loops:
        parser.exit(1, "No main-loop events recorded.\n")
    cutoff = min(event["ts"] for event in loops) + args.skip_seconds * 1e6
    loops = [event for event in loops if event["ts"] >= cutoff]
    if not loops:
        parser.exit(1, "No main-loop events remain after the selected cutoff.\n")
    events = [event for event in events if event["ts"] >= cutoff]
    durations = [event["dur"] / 1000 for event in loops]
    spikes = [event for event in loops if event["dur"] / 1000 >= args.spike_ms]
    print(f"Frames: {len(loops)}; frames >= {args.spike_ms:g} ms: {len(spikes)}")
    print("Main loop ms: " + ", ".join(
        f"p{int(fraction * 100)}={percentile(durations, fraction):.3f}"
        for fraction in (0.5, 0.95, 0.99)) + f", max={max(durations):.3f}")
    print(f"Dropped trace events: {trace.get('droppedEvents', 0)}")
    grouped = defaultdict(list)
    for event in events:
        grouped[event["name"]].append(event["dur"] / 1000)
    print("\nLongest phases by p99 (nested phases and different threads are not additive):")
    print(f"{'Phase':48} {'Count':>7} {'p50 ms':>10} {'p99 ms':>10} {'Max ms':>10}")
    for name, values in sorted(grouped.items(), key=lambda item: percentile(item[1], 0.99), reverse=True)[:18]:
        print(f"{name:48} {len(values):7} {percentile(values, 0.5):10.3f} "
              f"{percentile(values, 0.99):10.3f} {max(values):10.3f}")
    maintenance = overlap_index(event for event in events if event["name"] == "Resource cache maintenance")
    print(f"\nSlow frames overlapping resource maintenance: "
          f"{sum(overlaps(maintenance, event) for event in spikes)}/{len(spikes)}")
    print("Overlap alone does not establish a cause. Compare lock-wait and swap events in the trace.")
    print("\nWorst frames:")
    for event in sorted(loops, key=lambda item: item["dur"], reverse=True)[:10]:
        print(f"frame={event.get('args', {}).get('frame', '?')} "
              f"time={event['ts'] / 1e6:.3f}s duration={event['dur'] / 1000:.3f}ms "
              f"cache_overlap={overlaps(maintenance, event)}")


if __name__ == "__main__":
    main()
