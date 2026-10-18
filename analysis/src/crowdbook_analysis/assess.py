"""From a recording to a verdict in one command: fit a spec to the recording, check the fitted
markets for edges the recording does not show, and say whether the fit can be trusted for
testing strategies, and if not, why not.

    uv run --project analysis crowdbook-assess examples/calibration/reference_twins_spec.toml \\
        --real RECORDED_MARKETS --output assessed

It runs `crowdbook-calibrate` (the fit, its validation report and, for twins, every recorded
market's fitted twin), then `crowdbook-sweep` on the fitted markets, and writes `verdict.md`
beside their outputs. A fit is usable when it reaches the recording within chance and no generic
strategy makes money in it; the verdict lists the statistics furthest from the recording, any
strategy the fitted market pays, and what a standing quote earns in the recording against the
fit, the real side of a maker's flag. The command exits with 0 for a usable fit and 1 otherwise.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

from crowdbook_analysis import calibrate, sweep
from crowdbook_analysis.runner import find_tool

# Ticks from the mid of the standing quotes the report measures, and the horizon they are marked.
QUOTES = (1, 3, 8)


def verdict(data: dict, swept: dict | None, search: int, confirm: int) -> tuple[str, bool]:
    """The verdict as Markdown, and whether the fit is usable, from the fit's report data and the
    sweep's results (None where it was not run)."""
    fit = data["calibration"]
    compared = set(fit["compared"])
    chance = calibrate.chi_squared_95(len(compared))
    rows = []
    for entry in data["statistics"]:
        fitted = entry["simulated"][-1]
        if entry["key"] in compared and fitted.get("distance") is not None:
            rows.append((abs(fitted["distance"]), entry, fitted))
    passing = sum(1 for _, _, fitted in rows if fitted.get("pass"))
    flagged = [] if swept is None else [p for p, s in swept["judged"].items() if s["low"] > 0]
    reasons = []
    if not fit["reached"]:
        reasons.append(f"the fit does not reach the recording within chance (held-out distance "
                       f"{fit['distance_held_out']:,.1f} against {chance:,.1f})")
    if flagged:
        reasons.append("the fitted market pays a generic strategy: "
                       + ", ".join(p.name for p in flagged))
    if swept is None:
        reasons.append("the no-false-edge check was not run")
    usable = not reasons
    lines = ["# Verdict", "",
             ("**Usable for testing strategies**: the fit reaches the recording within chance "
              "and no generic strategy makes money in it." if usable else
              "**Not yet usable for testing strategies**: " + "; ".join(reasons) + "."), "",
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
    if swept is None:
        lines.append("Not checked.")
    else:
        lines.append(f"Each family's best of {search} markets, judged on {confirm} fresh ones; "
                     "tick-lots a market after fees, with 95% intervals.")
        lines += ["", "| strategy | made a market | flagged |", "|---|---:|---|"]
        for probe, s in swept["judged"].items():
            lines.append(f"| {probe.name} | {s['mean']:+,.1f} [{s['low']:+,.1f}, "
                         f"{s['high']:+,.1f}] | {'**edge**' if s['low'] > 0 else ''} |")
    by_key = {entry["key"]: entry for entry in data["statistics"]}
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
    parser.add_argument("--sweep-seeds", type=int, default=100,
                        help="markets for each strategy in the search (100)")
    parser.add_argument("--confirm", type=int, default=400,
                        help="fresh markets for each family's best (400)")
    parser.add_argument("--families", help="only these strategy families, such as maker,trend")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)

    command = [sys.executable, "-m", "crowdbook_analysis.calibrate", str(args.spec),
               "--real", str(args.real), "--crowdbook", str(tool), "--output", str(args.output),
               "--refits", str(args.refits)]
    if args.start:
        command += ["--start", str(args.start)]
    for name in ("generations", "seeds", "held_out"):
        if getattr(args, name) is not None:
            command += [f"--{name.replace('_', '-')}", str(getattr(args, name))]
    if subprocess.run(command).returncode != 0:
        raise SystemExit("the fit failed")
    data = json.loads((args.output / "report.json").read_text())

    twins = args.output / "twins"
    files = sorted(twins.glob("*.toml")) if twins.is_dir() else [args.output / "scenario.toml"]
    probes = sweep.default_probes()
    if args.families:
        wanted = set(args.families.split(","))
        probes = [p for p in probes if p.family in wanted]
    swept = sweep.sweep(tool, [f.read_text() for f in files], probes, args.sweep_seeds,
                        args.confirm)
    text, _ = sweep.report(f"{len(files)} fitted markets", swept, args.sweep_seeds, args.confirm)
    (args.output / "sweep.md").write_text(text)

    text, usable = verdict(data, swept, args.sweep_seeds, args.confirm)
    (args.output / "verdict.md").write_text(text)
    print(text)
    raise SystemExit(0 if usable else 1)


if __name__ == "__main__":
    main()
