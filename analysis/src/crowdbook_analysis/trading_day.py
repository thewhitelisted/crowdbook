"""Measures how a trading day's activity curve shapes volume, volatility and spreads, and plots it.

    uv run --project analysis crowdbook-trading-day

Runs examples/scenarios/trading_day.toml as a full day of six and a half hours, under several
seeds, with its U-shaped activity curve and with a flat one, and measures through the continuous
session, in equal slices of the day: the lots traded per minute, the volatility of one-minute
changes in the mid, and the time-weighted spread. The noise traders' order rates follow the curve
by assumption; the volatility and spread curves are what the market makes of it.

The chart goes to docs/images; the numbers are printed as Markdown.
"""

import argparse
import os
import re
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import style
from crowdbook_analysis.log import read_log, read_prices
from crowdbook_analysis.runner import REPO, Run, find_tool, run

MARKET = REPO / "examples" / "scenarios" / "trading_day.toml"
DAY_SECONDS = 23_400  # six and a half hours, as on a US exchange
AUCTION_SECONDS = 60  # each of the example's opening and closing auctions
SLICES = 13  # half-hour slices of the continuous session
SEEDS = 16
CURVES = {"U-shaped activity (2.0)": 2.0, "flat activity": 0.0}


def scenario_with(activity: float) -> str:
    """The example market as a full day, with the given activity curve."""
    text = MARKET.read_text()
    text = re.sub(r'^duration = ".*"', f'duration = "{DAY_SECONDS}s"', text, flags=re.M)
    return re.sub(r"^activity = [0-9.]+", f"activity = {activity}", text, flags=re.M)


def measure(tool: Path, job: Run, scratch: Path, index: int) -> dict:
    """One day's curves: per slice of the continuous session, lots per minute, the standard
    deviation of one-minute mid changes in ticks, and the mean spread in ticks."""
    log_path = scratch / f"day{index}.csv"
    prices_path = scratch / f"prices{index}.csv"
    run(tool, job, log=log_path, log_only="trade", prices=prices_path)
    trades = read_log(log_path).filter(pl.col("liquidity").is_null())
    prices = read_prices(prices_path)
    open_ns = AUCTION_SECONDS * 1_000_000_000
    close_ns = (DAY_SECONDS - AUCTION_SECONDS) * 1_000_000_000
    edges = np.linspace(open_ns, close_ns, SLICES + 1)

    volume = np.histogram(trades["time"].to_numpy(), bins=edges,
                          weights=trades["quantity"].to_numpy())[0]
    minutes_per_slice = (close_ns - open_ns) / SLICES / 60e9

    # Mids at whole minutes, from the one-second samples.
    sampled = prices.filter((pl.col("time") % 60_000_000_000 == 0)
                            & (pl.col("time") >= open_ns) & (pl.col("time") <= close_ns))
    minute_times = sampled["time"].to_numpy()[1:]
    changes = np.diff(sampled["mid"].to_numpy())
    slice_of_change = np.digitize(minute_times, edges) - 1
    volatility = np.array([np.nanstd(changes[slice_of_change == s]) for s in range(SLICES)])

    continuous = prices.filter((pl.col("time") >= open_ns) & (pl.col("time") < close_ns))
    spreads = (continuous["ask"] - continuous["bid"]).to_numpy()
    slice_of_sample = np.digitize(continuous["time"].to_numpy(), edges) - 1
    spread = np.array([np.nanmean(spreads[slice_of_sample == s]) for s in range(SLICES)])
    return {"volume": volume / minutes_per_slice, "volatility": volatility, "spread": spread}


def measure_all(tool: Path, jobs: list[Run]) -> list[dict]:
    with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(
        max_workers=os.cpu_count()
    ) as pool:
        return list(pool.map(lambda pair: measure(tool, pair[1], Path(scratch), pair[0]),
                             enumerate(jobs)))


def summarize(days: list[dict]) -> dict:
    """Means over days, with standard errors, of each curve."""
    summary = {}
    for name in ("volume", "volatility", "spread"):
        stacked = np.vstack([day[name] for day in days])
        summary[name] = (stacked.mean(axis=0),
                         stacked.std(axis=0, ddof=1) / np.sqrt(stacked.shape[0]))
    return summary


def plot(summaries: dict, path: Path) -> None:
    names = {"volume": "lots a minute", "volatility": "volatility", "spread": "spread"}
    fig, axes = style.panels("Through the trading day, against each curve's day average",
                             list(names.values()), "hours",
                             "relative to the day's average")
    hours = (np.arange(SLICES) + 0.5) * (DAY_SECONDS - 2 * AUCTION_SECONDS) / SLICES / 3600
    for ax, name in zip(axes, names, strict=True):
        for slot, (label, summary) in enumerate(summaries.items()):
            mean = summary[name][0]
            style.line(ax, hours, mean / mean.mean(), slot, label, markers=False)
    style.finish(fig, axes, path, end_labels=False)


def print_table(summaries: dict) -> None:
    thirds = np.array_split(np.arange(SLICES), 3)
    print("| activity | measure | first third | middle third | last third | first / middle |")
    print("|---|---|---:|---:|---:|---:|")
    for label, summary in summaries.items():
        for name, unit in (("volume", "lots a minute"), ("volatility", "ticks, 1 min"),
                           ("spread", "ticks")):
            mean = summary[name][0]
            parts = [mean[third].mean() for third in thirds]
            print(f"| {label} | {name} ({unit}) | "
                  + " | ".join(f"{part:.2f}" for part in parts)
                  + f" | {parts[0] / parts[1]:.2f} |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=SEEDS)
    parser.add_argument("--images", type=Path, default=REPO / "docs" / "images")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)

    summaries = {}
    for label, activity in CURVES.items():
        jobs = [Run(scenario_with(activity), seed) for seed in range(1, args.seeds + 1)]
        summaries[label] = summarize(measure_all(tool, jobs))
    print_table(summaries)
    plot(summaries, args.images / "trading_day.png")


if __name__ == "__main__":
    main()
