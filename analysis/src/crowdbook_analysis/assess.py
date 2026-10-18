"""From a recording to a verdict in one command: fit a spec to the recording, check the fitted
markets for edges the recording does not show, and say whether the fit can be trusted for
testing strategies, and if not, why not.

    uv run --project analysis crowdbook-assess examples/calibration/reference_twins_spec.toml \\
        --real RECORDED_MARKETS --output assessed

It runs `crowdbook-calibrate` (the fit, its validation report and, for twins, every recorded
market's fitted twin), then `crowdbook-sweep` on the fitted markets, and writes `verdict.md`
beside their outputs. A fit is usable when it reaches the recording within chance and no generic
strategy, nor a small learner trained in it, makes money in it; the verdict lists the statistics furthest from the recording, any
strategy the fitted market pays, and what a standing quote earns in the recording against the
fit, the real side of a maker's flag. The command exits with 0 for a usable fit and 1 otherwise.
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

from crowdbook_analysis import calibrate, sweep
from crowdbook_analysis.runner import find_tool

# Ticks from the mid of the standing quotes the report measures, and the horizon they are marked.
QUOTES = (1, 3, 8)


def shared_by_the_recording(probe, by_key: dict) -> str | None:
    """For a maker probe the fit pays, what the recording says of a standing quote about as wide:
    where it earns a fill there too, and within two tolerances of what it earns in the fit, the
    flag may be the market's own edge rather than the fit's. None where the recording does not
    say so, or for a probe that is not a maker."""
    if probe.family != "maker":
        return None
    offset = float(dict(probe.settings).get("offset", 1.0))
    k = min(QUOTES, key=lambda q: abs(q - offset))
    entry = by_key.get(f"quote_{k}_after_60")
    if entry is None:
        return None
    real, fitted = entry["real"], entry["simulated"][-1]
    distance = fitted.get("distance")
    if not (real is not None and math.isfinite(real) and real > 0 and distance is not None
            and math.isfinite(distance) and abs(distance) <= 2):
        return None
    return (f"the recording pays a quote {k} ticks from the mid as much ({real:+.2f} ticks a "
            f"fill, marked 60 s later, against {fitted['value']:+.2f} in the fit), so the flag "
            f"of {probe.name} may be the market's own edge, not the fit's")


def verdict(data: dict, swept: dict | None, search: int, confirm: int) -> tuple[str, bool]:
    """The verdict as Markdown, and whether the fit is usable, from the fit's report data and the
    sweep's results (None where it was not run)."""
    fit = data["calibration"]
    compared = set(fit["compared"])
    chance = calibrate.chi_squared_95(len(compared))
    rows = []
    for entry in data["statistics"]:
        fitted = entry["simulated"][-1]
        distance = fitted.get("distance")
        if entry["key"] in compared and distance is not None and math.isfinite(distance):
            rows.append((abs(distance), entry, fitted))
    passing = sum(1 for _, _, fitted in rows if fitted.get("pass"))
    # A check that judged nothing, or no strategy with an interval, did not run.
    judged = {} if swept is None else {p: s for p, s in swept["judged"].items()
                                       if math.isfinite(s["low"])}
    flagged = [p for p, s in judged.items() if s["low"] > 0]
    by_key = {entry["key"]: entry for entry in data["statistics"]}
    shared = [note for note in (shared_by_the_recording(p, by_key) for p in flagged) if note]
    reasons = []
    if not fit["reached"]:
        reasons.append(f"the fit does not reach the recording within chance (held-out distance "
                       f"{fit['distance_held_out']:,.1f} against {chance:,.1f})")
    if flagged:
        reasons.append("the fitted market pays a strategy: "
                       + ", ".join(p.name for p in flagged))
    if not judged:
        reasons.append("the no-false-edge check judged no strategy with an interval")
    usable = not reasons
    note = ("" if not shared else
            " Yet " + "; and ".join(shared) + ": a flag the recording shares says the market "
            "pays it, which a strategy tested in the fit should expect, rather than that the fit "
            "is wrong.")
    lines = ["# Verdict", "",
             ("**Usable for testing strategies**: the fit reaches the recording within chance "
              "and no strategy of the check makes money in it." if usable else
              "**Not yet usable for testing strategies**: " + "; ".join(reasons) + "." + note),
             "",
             "## The fit", "",
             f"- Held-out distance {fit['distance_held_out']:,.1f}, against {chance:,.1f} that "
             f"chance alone stays under 95% of the time, over {len(compared)} statistics.",
             f"- {passing} of {len(rows)} compared statistics within 2 tolerances of the "
             "recording.",
             f"- {fit['runs']:,} markets run.", ""]
    worst = sorted(rows, key=lambda row: -row[0])[:8]
    if worst:
        lines += ["Furthest from the recording:", "",
                  "| statistic | recording | fit | distance |", "|---|---:|---:|---:|"]
        for distance, entry, fitted in worst:
            lines.append(f"| {entry['label']} | {entry['real']:.4g} | {fitted['value']:.4g} | "
                         f"{fitted['distance']:+.1f} |")
        lines.append("")
    lines += ["## Edges", ""]
    if not judged:
        lines.append("Not checked: no strategy was judged on enough fresh markets for an "
                     "interval.")
    else:
        learned = "learner" in swept
        lines.append(f"Each family's best of {search} markets"
                     + (", and a learner trained in the fitted markets," if learned else "")
                     + f" judged on {confirm} fresh ones; tick-lots a market after fees, with "
                     "95% intervals.")
        lines += ["", "| strategy | made a market | flagged |", "|---|---:|---|"]
        for probe, s in swept["judged"].items():
            lines.append(f"| {probe.name} | {s['mean']:+,.1f} [{s['low']:+,.1f}, "
                         f"{s['high']:+,.1f}] | {'**edge**' if s['low'] > 0 else ''} |")
        if learned:
            lines += ["", *("#" + line if line.startswith("## ") else line
                            for line in sweep.learner_section(swept["learner"]))]
    quote_rows = []
    for k in QUOTES:
        for suffix, what in (("after_60", "marked 60 s later"), ("settlement", "at settlement")):
            entry = by_key.get(f"quote_{k}_{suffix}")
            if entry is None:
                continue
            fitted = entry["simulated"][-1]["value"]
            quote_rows.append(f"| {k} ticks out, {what} | {_cell(entry['real'])} | "
                              f"{_cell(fitted)} |")
    if quote_rows:
        lines += ["", "What a lot quoted around the mid earns a fill, in ticks: where the fit pays "
                  "a quote much more than the recording, a maker's flag is the fit's.", "",
                  "| quote | recording | fit |", "|---|---:|---:|"] + quote_rows
    return "\n".join(lines) + "\n", usable


def _cell(value) -> str:
    return "–" if value is None or value != value else f"{value:+.2f}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path)
    parser.add_argument("--real", type=Path, required=True, help="the recording's tape")
    parser.add_argument("--output", type=Path, default=Path("assessed"))
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--generations", type=int)
    parser.add_argument("--seeds", type=int)
    parser.add_argument("--held-out", type=int)
    parser.add_argument("--refits", type=int, default=0)
    parser.add_argument("--start", type=Path, metavar="SETTINGS",
                        help="start the fit from an earlier fit's settings.toml")
    parser.add_argument("--step", type=float, help="the search's first step (see calibrate)")
    parser.add_argument("--sweep-seeds", type=int, default=100,
                        help="markets for each strategy in the search (100)")
    parser.add_argument("--confirm", type=int, default=400,
                        help="fresh markets for each family's best (400)")
    parser.add_argument("--families", help="only these strategy families, such as maker,trend")
    sweep.add_learner_arguments(parser)
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)

    command = [sys.executable, "-m", "crowdbook_analysis.calibrate", str(args.spec),
               "--real", str(args.real), "--crowdbook", str(tool), "--output", str(args.output),
               "--refits", str(args.refits)]
    if args.start:
        command += ["--start", str(args.start)]
    if args.step is not None:
        command += ["--step", str(args.step)]
    for name in ("generations", "seeds", "held_out"):
        if getattr(args, name) is not None:
            command += [f"--{name.replace('_', '-')}", str(getattr(args, name))]
    if subprocess.run(command).returncode != 0:
        raise SystemExit("the fit failed")
    data = json.loads((args.output / "report.json").read_text())

    # The fit's own markets: its twins, which the fit writes afresh, or its one scenario.
    conditioned = calibrate.read_spec(args.spec).condition
    files = (sorted((args.output / "twins").glob("*.toml")) if conditioned
             else [args.output / "scenario.toml"])
    probes = sweep.default_probes()
    if args.families:
        wanted = set(args.families.split(","))
        probes = [p for p in probes if p.family in wanted]
        if not probes:
            known = sorted({p.family for p in sweep.default_probes()})
            parser.error(f"no strategy family in {args.families}: they are {', '.join(known)}")
    # Every fitted market at least once in the search and twice on the fresh markets.
    search = max(args.sweep_seeds, len(files))
    confirm = max(args.confirm, 2 * len(files))
    swept = sweep.sweep(tool, [f.read_text() for f in files], probes, search, confirm,
                        learner=sweep.learner_settings(args))
    text, _ = sweep.report(f"{len(files)} fitted markets", swept, search, confirm)
    (args.output / "sweep.md").write_text(text)

    text, usable = verdict(data, swept, search, confirm)
    (args.output / "verdict.md").write_text(text)
    print(text)
    raise SystemExit(0 if usable else 1)


if __name__ == "__main__":
    main()
