"""Market-maker experiments: how its PnL depends on its quote skew and on informed flow, and
where that PnL comes from.

    uv run --project analysis crowdbook-experiments

Each point averages many seeds of a 60-second market; error bars are one standard error. Charts
go to docs/images; the numbers are printed as Markdown.
"""

import argparse
import os
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import polars as pl

from crowdbook_analysis import facts, style
from crowdbook_analysis.facts import mean_and_error
from crowdbook_analysis.log import executions, quotes, read_log
from crowdbook_analysis.runner import (
    REPO,
    Run,
    agent_groups,
    find_tool,
    group,
    pnl_at_value,
    run,
    run_all,
)

MAKER = """
[[agents]]
type = "market_maker"
name = "maker"
risk_aversion = {risk_aversion}
volatility = 3.0
intensity = {intensity}
quote_size = 10
max_inventory = 200
requote_interval = "50ms"
latency = {{ to_exchange = "{latency}", from_exchange = "{latency}" }}
account = {{ max_position = 220, max_order_quantity = 10 }}
"""

NOISE = """
[[agents]]
type = "zero_intelligence"
name = "noise"
count = 20
limit_rate = {limit_rate}
market_rate = 1.0
cancel_rate = 0.2
max_offset = 8
latency = {{ to_exchange = "300us", from_exchange = "300us", jitter = "100us" }}
account = {{ max_position = 1000, max_order_quantity = 10 }}
"""

INFORMED = """
[[agents]]
type = "informed"
name = "informed"
count = {count}
interval = "50ms"
noise = 1.0
threshold = 2.0
order_size = 5
max_position = 1000
latency = {{ to_exchange = "100us", from_exchange = "100us", jitter = "20us" }}
account = {{ max_position = 1010, max_order_quantity = 5 }}
"""

HEADER = """
duration = "60s"
reference_price = 10000
"""

FUNDAMENTAL = """
[fundamental]
volatility = 4.0
step = "100ms"
"""

QUOTES = {"quotes about 3 ticks out (k = 0.3)": 0.3, "quotes at the touch (k = 1.0)": 1.0}

SKEW_PER_GAMMA = 3.0**2 * 1.0  # σ²τ: the maker's volatility squared times its 1 s horizon
LOWEST_SHOWN = -6.0  # ticks per lot; points below are labelled at the bottom of the chart


def per_lot(pnl: float, traded: int) -> float:
    return pnl / traded if traded > 0 else float("nan")


def pnl_per_lot(runs: list[dict], name: str) -> tuple:
    """The mean and standard error over runs of a group's PnL per lot traded, with positions
    valued at the true value when there is one."""
    values = []
    for result in runs:
        team = group(result, name)
        value = result["final_value"] if result["final_value"] is not None else result["last_price"]
        values.append(per_lot(pnl_at_value(team, value), team["traded"]))
    return mean_and_error(values)


def self_impact(tool: Path, seeds: int, out: Path) -> None:
    """The market maker against noise traders only, sweeping its risk aversion (how far
    inventory skews its quotes) and how much liquidity the noise traders post themselves."""
    risk_aversions = [0.0025, 0.005, 0.01, 0.02, 0.04, 0.08]
    liquidity = {"thin": 0.5, "medium": 1.0, "deep": 2.0}  # limit orders per second per trader
    cells = [(name, rate, risk) for name, rate in liquidity.items() for risk in risk_aversions]
    jobs = [
        Run(
            HEADER
            + MAKER.format(risk_aversion=risk, intensity=0.3, latency="20us")
            + NOISE.format(limit_rate=rate),
            seed,
        )
        for _, rate, risk in cells
        for seed in range(1, seeds + 1)
    ]
    results = run_all(tool, jobs)

    fig, ax = style.figure(
        "Market maker PnL as its quote skew grows",
        "quote skew per lot of inventory, γσ²τ (ticks)",
        "PnL per lot traded (ticks)",
    )
    skews = [risk * SKEW_PER_GAMMA for risk in risk_aversions]
    highest = 0.0
    print("| noise liquidity | risk aversion γ | skew per lot (ticks) | "
          "maker PnL per lot (ticks) |")
    print("|---|---:|---:|---:|")
    for slot, name in enumerate(liquidity):
        means, errors = [], []
        for index, (cell_name, _, risk) in enumerate(cells):
            if cell_name != name:
                continue
            runs = results[index * seeds : (index + 1) * seeds]
            mean, error = pnl_per_lot(runs, "maker")
            means.append(mean)
            errors.append(error)
            highest = max(highest, mean + error)
            skew = risk * SKEW_PER_GAMMA
            print(f"| {name} | {risk} | {skew:.3f} | {mean:+.3f} ± {error:.3f} |")
        style.line(ax, skews, means, slot, f"{name} noise liquidity", error=errors)
        for skew, mean in zip(skews, means, strict=True):
            if mean < LOWEST_SHOWN:
                ax.annotate(
                    f"{mean:.0f} ↓",
                    (skew, LOWEST_SHOWN),
                    xytext=(0, 4),
                    textcoords="offset points",
                    ha="center",
                    color=style.INK_SECONDARY,
                    fontsize=8,
                )
    ax.set_xscale("log")
    ax.set_xticks(skews, labels=[f"{skew:.2g}" for skew in skews])
    ax.minorticks_off()
    ax.set_ylim(LOWEST_SHOWN, highest + 0.5)
    ax.axhline(0, color=style.AXIS, linewidth=style.HAIRLINE)
    style.finish(fig, ax, out / "maker_self_impact.png", end_labels=False)


def adverse_selection(tool: Path, seeds: int, out: Path) -> None:
    """The market maker with noise traders and a growing number of informed traders, quoting
    wide or at the touch, at three connection speeds. PnL values positions at the true value."""
    latencies = {"20 µs": "20us", "500 µs": "500us", "5 ms": "5ms"}
    counts = [0, 2, 5, 10, 20]
    cells = [(q, latency, count) for q in QUOTES for latency in latencies for count in counts]
    jobs = [
        Run(
            HEADER
            + FUNDAMENTAL
            + MAKER.format(risk_aversion=0.005, intensity=QUOTES[q], latency=latencies[latency])
            + NOISE.format(limit_rate=1.0)
            + (INFORMED.format(count=count) if count > 0 else ""),
            seed,
        )
        for q, latency, count in cells
        for seed in range(1, seeds + 1)
    ]
    results = run_all(tool, jobs)
    runs = {cell: results[index * seeds : (index + 1) * seeds] for index, cell in enumerate(cells)}

    fig, axes = style.panels(
        "Who pays for informed trading",
        list(QUOTES),
        "informed traders",
        "PnL per lot traded (ticks, at the true value)",
    )
    print("| maker's quotes | informed traders | maker PnL per lot at "
          + " | at ".join(latencies) + " | maker lots per minute | noise traders' PnL per lot | "
          "informed traders' PnL per lot |")
    print("|---|---:|" + "---:|" * (len(latencies) + 3))
    fastest = next(iter(latencies))
    for ax, q in zip(axes, QUOTES, strict=True):
        series = {"noise traders": [], "market maker": [], "informed traders": []}
        for count in counts:
            fast = runs[(q, fastest, count)]
            makers = [pnl_per_lot(runs[(q, latency, count)], "maker") for latency in latencies]
            lots = sum(group(result, "maker")["traded"] for result in fast) / len(fast)
            noise = pnl_per_lot(fast, "noise")
            informed = pnl_per_lot(fast, "informed") if count > 0 else None
            series["market maker"].append(makers[0])
            series["noise traders"].append(noise)
            series["informed traders"].append(informed)
            print(f"| {q} | {count} | "
                  + " | ".join(f"{mean:+.2f} ± {error:.2f}" for mean, error in makers)
                  + f" | {lots:,.0f} | {noise[0]:+.2f} ± {noise[1]:.2f} | "
                  + (f"{informed[0]:+.2f} ± {informed[1]:.2f}" if informed else "—") + " |")
        ax.axhline(0, color=style.AXIS, linewidth=style.HAIRLINE)
        for slot, (label, points) in enumerate(series.items()):
            shown = [(count, p) for count, p in zip(counts, points, strict=True) if p is not None]
            style.line(
                ax,
                [count for count, _ in shown],
                [p[0] for _, p in shown],
                slot,
                label,
                error=[p[1] for _, p in shown],
            )
    style.finish(fig, axes, out / "maker_adverse_selection.png")


def maker_markouts(tool: Path, seeds: int) -> None:
    """Where the market maker's PnL comes from: how far the mid moves in its favour after each of
    its fills, per lot, split by who took the other side, with and without informed traders."""
    horizons = {"1 ms": 1_000_000, "1 s": 1_000_000_000, "10 s": 10_000_000_000}
    cases = [(q, count) for q in QUOTES for count in (0, 20)]

    def measure(case: tuple, seed: int) -> pl.DataFrame:
        q, count = case
        text = (
            HEADER
            + FUNDAMENTAL
            + MAKER.format(risk_aversion=0.005, intensity=QUOTES[q], latency="20us")
            + NOISE.format(limit_rate=1.0)
            + (INFORMED.format(count=count) if count > 0 else "")
        )
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "log.csv"
            result = run(tool, Run(text, seed), log=path, log_only="filled,top_of_book")
            log = read_log(path)
        owners = agent_groups(result)
        fills = executions(log).with_columns(
            pl.col("maker", "taker").replace_strict(owners, return_dtype=pl.Utf8)
        ).filter(pl.col("maker") == "maker")
        marked = facts.markouts(fills, quotes(log), list(horizons.values()), result["duration_ns"])
        return (
            marked.group_by("taker", "horizon_ns")
            .agg(
                ((pl.col("markout") * pl.col("quantity")).sum() / pl.col("quantity").sum()),
                pl.col("quantity").sum().alias("lots"),
            )
            .with_columns(seed=pl.lit(seed))
        )

    jobs = [(case, seed) for case in cases for seed in range(1, seeds + 1)]
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        frames = list(pool.map(lambda job: measure(*job), jobs))

    print("| maker's quotes | informed traders | other side | share of the maker's lots | "
          + " | ".join(f"markout per lot at {label}" for label in horizons) + " |")
    print("|---|---:|---|---:|" + "---:|" * len(horizons))
    for index, (q, count) in enumerate(cases):
        runs = pl.concat(frames[index * seeds : (index + 1) * seeds])
        first = runs.filter(pl.col("horizon_ns") == next(iter(horizons.values())))
        whole = first["lots"].sum()
        for taker in sorted(first["taker"].unique()):
            share = first.filter(pl.col("taker") == taker)["lots"].sum() / whole
            cells = []
            for horizon in horizons.values():
                chosen = (pl.col("taker") == taker) & (pl.col("horizon_ns") == horizon)
                per_seed = runs.filter(chosen)
                mean, error = mean_and_error(per_seed["markout"].to_list())
                cells.append(f"{mean:+.2f} ± {error:.2f}")
            print(f"| {q} | {count} | {taker} traders | {share:.0%} | " + " | ".join(cells) + " |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="path to the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=32, help="runs per point")
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "images")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)
    self_impact(tool, args.seeds, args.out)
    print()
    adverse_selection(tool, args.seeds, args.out)
    print()
    maker_markouts(tool, args.seeds)


if __name__ == "__main__":
    main()
