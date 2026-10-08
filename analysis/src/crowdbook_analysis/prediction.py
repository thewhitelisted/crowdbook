"""Checks that a prediction market's prices are calibrated probabilities, and measures how fast they
move as resolution nears, and plots both.

    uv run --project analysis crowdbook-prediction

Runs examples/scenarios/prediction.toml under many seeds. Every thirty seconds of each run it takes
the mid price and the true probability, and compares both with how the question resolved: of the
moments the market said 70, about 70% should resolve yes. It also measures the size of one-minute
moves in the mid through the run, for markets still close to a coin flip and for ones nearly
decided, since a price heading for 0 or 100 moves fastest at the end only when the question is
still open.

The charts go to docs/images; the numbers are printed as Markdown.
"""

import argparse
import os
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

from crowdbook_analysis import style
from crowdbook_analysis.log import read_prices
from crowdbook_analysis.runner import REPO, Run, find_tool, run

MARKET = REPO / "examples" / "scenarios" / "prediction.toml"
SEEDS = 400
EVERY = 30  # seconds between the moments compared with the outcome
MOVE = 60  # seconds over which a move is measured
TENTHS = 10


def measure(tool: Path, job: Run, scratch: Path, index: int) -> dict:
    """One run's mids and probabilities each second, and how it resolved (1 for yes)."""
    prices_path = scratch / f"prices{index}.csv"
    result = run(tool, job, prices=prices_path)
    prices = read_prices(prices_path)
    return {"mid": prices["mid"].to_numpy(), "value": prices["value"].to_numpy(),
            "yes": result["final_value"] / 100.0}


def measure_all(tool: Path, jobs: list[Run]) -> list[dict]:
    with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(
        max_workers=os.cpu_count()
    ) as pool:
        return list(pool.map(lambda pair: measure(tool, pair[1], Path(scratch), pair[0]),
                             enumerate(jobs)))


def calibration(runs: list[dict], column: str) -> dict:
    """Moments binned by the price (or probability) then, with how often those runs resolved yes."""
    said, outcome = [], []
    for one in runs:
        moments = one[column][:-1:EVERY]  # every EVERY seconds, before resolution
        keep = ~np.isnan(moments)
        said.append(moments[keep] / 100.0)
        outcome.append(np.full(keep.sum(), one["yes"]))
    said, outcome = np.concatenate(said), np.concatenate(outcome)
    bins = np.minimum((said * 10).astype(int), 9)
    table = []
    for b in range(10):
        chosen = bins == b
        if chosen.sum() >= 30:
            table.append((said[chosen].mean(), outcome[chosen].mean(), int(chosen.sum())))
    return {"bins": table, "brier": float(np.mean((said - outcome) ** 2)),
            "moments": len(said), "yes": float(outcome.mean())}


def volatility(runs: list[dict]) -> dict:
    """The root mean square of one-minute moves in the mid, by tenth of the run: over all moves,
    over those starting while the question was close (probability 20 to 80), and over those
    starting when it was nearly decided (below 10 or above 90)."""
    length = len(runs[0]["mid"]) - 1
    sums = {name: np.zeros(TENTHS) for name in ("all", "close", "decided")}
    counts = {name: np.zeros(TENTHS) for name in ("all", "close", "decided")}
    for one in runs:
        mid, value = one["mid"], one["value"]
        start = np.arange(0, length - MOVE, MOVE)
        moves = mid[start + MOVE] - mid[start]
        tenth = np.minimum(start * TENTHS // length, TENTHS - 1)
        keep = ~np.isnan(moves)
        close = (value[start] >= 20) & (value[start] <= 80)
        decided = (value[start] < 10) | (value[start] > 90)
        for name, chosen in (("all", keep), ("close", keep & close),
                             ("decided", keep & decided)):
            np.add.at(sums[name], tenth[chosen], moves[chosen] ** 2)
            np.add.at(counts[name], tenth[chosen], 1)
    return {name: np.sqrt(sums[name] / np.maximum(counts[name], 1))
            for name in sums} | {f"{name}_count": counts[name] for name in counts}


def plot(prices: dict, values: dict, moves: dict, images: Path) -> None:
    """Two charts: calibration, and one-minute moves through the run."""
    fig, ax = style.figure("Prices are probabilities: how often the question resolved yes",
                           "price or probability, in cents", "resolved yes, %")
    style.reference(ax, [0, 100], [0, 100], "perfectly calibrated", dashed=True)
    for slot, (label, table) in enumerate((("mid price", prices), ("true probability", values))):
        style.line(ax, [row[0] * 100 for row in table["bins"]],
                   [row[1] * 100 for row in table["bins"]], slot, label)
    style.finish(fig, ax, images / "prediction_calibration.png", end_labels=False)

    fig, ax = style.figure("One-minute moves in the mid as resolution nears",
                           "share of the run elapsed", "cents, root mean square")
    tenths = (np.arange(TENTHS) + 0.5) / TENTHS
    for slot, (label, name) in enumerate((("every question", "all"),
                                          ("still close (20 to 80)", "close"),
                                          ("nearly decided (<10 or >90)", "decided"))):
        # Only where enough moves were measured.
        enough = moves[f"{name}_count"] >= 50
        style.line(ax, tenths[enough], moves[name][enough], slot, label)
    style.finish(fig, ax, images / "prediction_volatility.png", end_labels=False)


def print_tables(prices: dict, values: dict, moves: dict) -> None:
    print(f"{prices['moments']} moments from {SEEDS} runs; {prices['yes']:.1%} resolved yes\n")
    print("| price bin | mean price | resolved yes | moments |")
    print("|---|---:|---:|---:|")
    for said, happened, count in prices["bins"]:
        low = int(said * 10) * 10
        print(f"| {low} to {low + 10} | {said * 100:.1f} | {happened * 100:.1f}% | {count} |")
    print()
    print("| forecast | Brier score |")
    print("|---|---:|")
    print(f"| the true probability | {values['brier']:.4f} |")
    print(f"| the mid price | {prices['brier']:.4f} |")
    print(f"| a coin flip, 50 | {0.25:.4f} |")
    print()
    print("| tenth of the run | all | close (20 to 80) | decided (<10 or >90) |")
    print("|---|---:|---:|---:|")
    for t in range(TENTHS):
        cells = []
        for name in ("all", "close", "decided"):
            enough = moves[f"{name}_count"][t] >= 50
            cells.append(f"{moves[name][t]:.2f}" if enough else "—")
        print(f"| {t + 1} | " + " | ".join(cells) + " |")


def main() -> None:
    global SEEDS
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=SEEDS)
    parser.add_argument("--images", type=Path, default=REPO / "docs" / "images")
    args = parser.parse_args()
    SEEDS = args.seeds
    tool = find_tool(args.crowdbook)

    text = MARKET.read_text()
    runs = measure_all(tool, [Run(text, seed) for seed in range(1, args.seeds + 1)])
    prices, values = calibration(runs, "mid"), calibration(runs, "value")
    moves = volatility(runs)
    print_tables(prices, values, moves)
    plot(prices, values, moves, args.images)


if __name__ == "__main__":
    main()
