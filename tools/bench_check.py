"""Runs the benchmarks and compares them with the recorded baseline, to catch a change that makes
any feature slower.

    python3 tools/bench_check.py                 # run, compare with bench/baseline.json
    python3 tools/bench_check.py --update        # run, and record the results as the baseline
    python3 tools/bench_check.py --update --filter Prediction   # record only these
    python3 tools/bench_check.py --tolerance 15  # allow 15% instead of 10%

It needs the release build (cmake --workflow --preset release). Each benchmark runs several times
and the median is compared, which keeps the noise of a quiet machine to a few percent. Timings
mean something only on the machine the baseline was recorded on, which the baseline names; on any
other, record a baseline first. Exits with 1 if anything is slower than the tolerance allows.
"""

import argparse
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BENCH = REPO / "build" / "release" / "bench" / "crowdbook_bench"
BASELINE = REPO / "bench" / "baseline.json"


def run(repetitions: int, filter_: str) -> dict:
    """Median real time per iteration of each benchmark, in nanoseconds."""
    with tempfile.NamedTemporaryFile(suffix=".json") as out:
        subprocess.run(
            [str(BENCH), f"--benchmark_repetitions={repetitions}",
             "--benchmark_report_aggregates_only=true", f"--benchmark_filter={filter_}",
             f"--benchmark_out={out.name}", "--benchmark_out_format=json"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        report = json.loads(Path(out.name).read_text())
    scale = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
    return {b["run_name"]: b["real_time"] * scale[b["time_unit"]]
            for b in report["benchmarks"] if b.get("aggregate_name") == "median"}


def duration(ns: float) -> str:
    if ns < 1e3:
        return f"{ns:.1f} ns"
    if ns < 1e6:
        return f"{ns / 1e3:.1f} us"
    return f"{ns / 1e6:.2f} ms"


def machine() -> str:
    """The processor's name, which is what timings depend on."""
    try:
        if platform.system() == "Darwin":
            return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    return f"{platform.system()} {platform.machine()}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--update", action="store_true", help="record the results as the baseline")
    parser.add_argument("--tolerance", type=float, default=10.0, help="percent slower allowed")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--filter", default=".", help="only benchmarks matching this regex")
    args = parser.parse_args()
    if not BENCH.exists():
        sys.exit(f"{BENCH} not found: build it with cmake --workflow --preset release")

    results = run(args.repetitions, args.filter)
    if args.update:
        # With a filter, only the benchmarks run are recorded; the rest of the baseline stays.
        recorded = {}
        if args.filter != "." and BASELINE.exists():
            recorded = json.loads(BASELINE.read_text())["ns"]
        recorded.update(results)
        BASELINE.write_text(json.dumps({"machine": machine(), "ns": recorded}, indent=1) + "\n")
        print(f"{len(results)} benchmarks recorded in {BASELINE.relative_to(REPO)}")
        return 0

    baseline = json.loads(BASELINE.read_text())
    if baseline["machine"] != machine():
        print(f"note: the baseline is from {baseline['machine']}, this is {machine()}")
    slower = 0
    print(f"{'benchmark':<58} {'baseline':>12} {'now':>12} {'change':>8}")
    for name, now in results.items():
        before = baseline["ns"].get(name)
        if before is None:
            print(f"{name:<58} {'—':>12} {duration(now):>12} {'new':>8}")
            continue
        change = (now / before - 1.0) * 100.0
        flag = ""
        if change > args.tolerance:
            flag = "  SLOWER"
            slower += 1
        print(f"{name:<58} {duration(before):>12} {duration(now):>12} {change:>+7.1f}%{flag}")
    for name in sorted(set(baseline["ns"]) - set(results)):
        if args.filter == ".":
            print(f"{name:<58} missing from this run")
    if slower:
        print(f"{slower} benchmark(s) more than {args.tolerance:g}% slower than the baseline")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
