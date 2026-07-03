#!/usr/bin/env python3
import argparse
import collections
import csv
import statistics


def percentile(values, fraction):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(int(len(values) * fraction), len(values) - 1)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_file")
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args()

    by_name = collections.defaultdict(list)
    by_frame = collections.defaultdict(float)

    with open(args.csv_file, newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                frame = int(row["frame"])
                gpu_ms = float(row["gpu_ms"])
            except (KeyError, ValueError):
                continue

            by_name[row["name"]].append(gpu_ms)
            by_frame[frame] += gpu_ms

    print(f"{args.csv_file}")
    print(f"scopes: {sum(len(v) for v in by_name.values())}")
    print(f"frames: {len(by_frame)}")
    if by_frame:
        frame_values = list(by_frame.values())
        print(
            "known scoped frame sum: "
            f"avg={statistics.mean(frame_values):.3f}ms "
            f"p95={percentile(frame_values, 0.95):.3f}ms "
            f"max={max(frame_values):.3f}ms"
        )

    print()
    print("avg_ms   p95_ms   max_ms   n       scope")
    for name, values in sorted(by_name.items(), key=lambda item: statistics.mean(item[1]), reverse=True)[: args.limit]:
        print(
            f"{statistics.mean(values):7.3f} "
            f"{percentile(values, 0.95):8.3f} "
            f"{max(values):8.3f} "
            f"{len(values):7d}  {name}"
        )


if __name__ == "__main__":
    main()
