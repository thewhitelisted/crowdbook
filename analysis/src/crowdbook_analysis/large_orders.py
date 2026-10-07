"""Measures what large orders worked over time do to a market, and plots it.

    uv run --project analysis crowdbook-large-orders

Runs examples/scenarios/large_orders.toml, where a hundred brokers work parent orders with child
market orders among a thousand noise traders, for an hour under each of several seeds with its
event log, and variants of it:

- parent sizes with a thinner tail, to test the predicted link between the tail of parent sizes
  and how long the signs of market orders stay correlated;
- noise traders whose limit orders rest ten and a hundred times longer, to see how the memory of
  the book shapes the price impact of a parent order.

Charts go to docs/images; the numbers are printed as Markdown.
"""

import argparse
import os
import re
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import facts, style
from crowdbook_analysis.log import aggressive_orders, parent_orders, quotes, read_log
from crowdbook_analysis.runner import REPO, Run, find_tool, group, run

MARKET = REPO / "examples" / "scenarios" / "large_orders.toml"
BROKERS = "brokers"
TAIL = 1.5  # parent_tail in the scenario
THIN_TAIL = 2.5

# Variants of the market, each a change of settings: key, old value as written, new value.
SIGN_VARIANTS = {
    f"parent sizes with tail {TAIL}": [],
    f"parent sizes with tail {THIN_TAIL}": [("parent_tail", TAIL, THIN_TAIL)],
}
MEMORY_VARIANTS = {
    "noise orders rest 5 s": [],
    "noise orders rest 50 s": [("cancel_rate", 0.2, 0.02)],
    "noise orders rest 500 s": [("cancel_rate", 0.2, 0.002)],
}

LAGS = np.unique(np.round(np.logspace(0, 4, 41)).astype(int))  # in market orders
FIT_LAGS = (30, 3000)  # beyond one child interval, and short of the longest parents
SHOWN_LAGS = [1, 10, 100, 1000]
SIZE_EDGES = np.unique(np.logspace(np.log10(15), np.log10(5000), 12).astype(int))
MIN_PARENTS = 50  # in a size bin, for it to count in the fit
SHOWN_SIZES = [(15, 30), (150, 300), (1500, 5000)]
BOOTSTRAPS = 200
WORKERS = max(1, min(4, os.cpu_count() or 1))  # each hour's log is a few hundred megabytes


def with_settings(text: str, changes: list[tuple]) -> str:
    """The scenario with each `key = old` line set to `key = new`; each must appear once."""
    for key, old, new in changes:
        text, count = re.subn(rf"(?m)^{key} = {re.escape(str(old))}\b.*$", f"{key} = {new}",
                              text)
        if count != 1:
            raise ValueError(f"the scenario does not set {key} = {old} exactly once")
    return text


def measure(tool: Path, job: Run, scratch: Path, index: int) -> dict:
    """Runs one hour with its event log; returns the autocorrelation of the signs of market
    orders, the brokers' alone and everyone's, and each parent order with its impact."""
    path = scratch / f"log-{index}.csv"
    result = run(tool, job, log=path, log_only="new,filled,top_of_book")
    log = read_log(path)
    path.unlink()
    orders = aggressive_orders(log)
    signs = np.where(orders["side"].to_numpy() == "buy", 1.0, -1.0)
    brokers = np.isin(orders["agent"].to_numpy(), group(result, BROKERS)["agent_ids"])
    # A parent still running near the end is cut short, so only those started in the first
    # three quarters of the run count; the longest take about six minutes. One that started
    # before both sides of the book were quoted has no impact to measure.
    parents = parent_orders(log).filter(
        pl.col("end").is_not_null() & (pl.col("start") < 0.75 * result["duration_ns"])
    )
    impact = facts.parent_impact(parents, quotes(log)).filter(pl.col("impact").is_not_nan())
    return {
        "broker_share": float(brokers.mean()),
        "acf_all": facts.autocorrelation_at(signs, LAGS),
        "acf_brokers": facts.autocorrelation_at(signs[brokers], LAGS),
        "parents": impact.select("lots", "impact"),
    }


def measure_all(tool: Path, jobs: list[Run]) -> list[dict]:
    with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(WORKERS) as pool:
        return list(
            pool.map(lambda pair: measure(tool, pair[1], Path(scratch), pair[0]),
                     enumerate(jobs))
        )


def resampled(runs: list, rng: np.random.Generator) -> list:
    """The runs drawn again with replacement, for a bootstrap over whole runs."""
    return [runs[i] for i in rng.integers(len(runs), size=len(runs))]


def summarize_signs(runs: list[dict]) -> dict:
    """The mean autocorrelation of signs over runs, and the exponent of its power-law decay,
    fitted over the lags in FIT_LAGS where the mean stands three standard errors above zero, with
    a standard error from resampling whole runs."""
    summary = {"broker_share": facts.mean_and_error([one["broker_share"] for one in runs])}
    rng = np.random.default_rng(1)
    for key in ("acf_all", "acf_brokers"):
        mean, error = facts.mean_and_error([one[key] for one in runs])
        used = (LAGS >= FIT_LAGS[0]) & (LAGS <= FIT_LAGS[1]) & (mean > 3 * error)
        bootstrapped = []
        for _ in range(BOOTSTRAPS):
            again = np.mean([one[key] for one in resampled(runs, rng)], axis=0)
            fitted = used & (again > 0)
            bootstrapped.append(-facts.log_log_slope(LAGS[fitted], again[fitted]))
        summary[key] = (mean, error)
        summary[f"{key}_exponent"] = (-facts.log_log_slope(LAGS[used], mean[used]),
                                      float(np.std(bootstrapped, ddof=1)))
        summary[f"{key}_range"] = (int(LAGS[used][0]), int(LAGS[used][-1]))
    return summary


def binned_impact(parents: pl.DataFrame) -> pl.DataFrame:
    """Mean impact and its standard error in each size bin, with the bin's mean size."""
    edges = list(SIZE_EDGES)
    return (
        parents.with_columns(bin=pl.col("lots").cut(edges[1:-1], left_closed=True))
        .filter(pl.col("lots").is_between(edges[0], edges[-1], closed="left"))
        .group_by("bin")
        .agg(
            pl.col("lots").mean().alias("size"),
            pl.col("impact").mean(),
            (pl.col("impact").std() / pl.len().sqrt()).alias("error"),
            pl.len().alias("parents"),
        )
        .sort("size")
    )


def impact_exponent(parents: pl.DataFrame) -> float:
    bins = binned_impact(parents).filter(
        (pl.col("parents") >= MIN_PARENTS) & (pl.col("impact") > 0)
    )
    return facts.log_log_slope(bins["size"].to_numpy(), bins["impact"].to_numpy())


def summarize_impact(runs: list[dict]) -> dict:
    """Every parent of the runs, their binned impact, and its exponent with a standard error from
    resampling whole runs, because the parents of one run share its price path."""
    pooled = pl.concat(one["parents"] for one in runs)
    rng = np.random.default_rng(1)
    exponents = [
        impact_exponent(pl.concat(one["parents"] for one in resampled(runs, rng)))
        for _ in range(BOOTSTRAPS)
    ]
    return {
        "pooled": pooled,
        "bins": binned_impact(pooled),
        "exponent": (impact_exponent(pooled), float(np.std(exponents, ddof=1))),
    }


def plot_signs(summaries: dict, path: Path) -> None:
    fig, ax = style.figure(
        "How long the signs of market orders stay correlated",
        "lag (market orders)",
        "autocorrelation of signs",
    )
    curves = {}
    for label, summary in summaries.items():
        curves[f"brokers' orders, {label}"] = summary["acf_brokers"]
    curves[f"all orders, {next(iter(summaries))}"] = next(iter(summaries.values()))["acf_all"]
    for slot, (label, (mean, error)) in enumerate(curves.items()):
        # Gaps where noise alone could give the value, which a log scale cannot show anyway.
        shown = np.where(mean > 2 * error, mean, np.nan)
        style.line(ax, LAGS, shown, slot, label, markers=False)
    start = int(np.argmax(LAGS >= FIT_LAGS[0]))
    anchor = next(iter(curves.values()))[0][start]
    guide = LAGS[(LAGS >= 10) & (LAGS <= 10_000)]
    for dashed, exponent in enumerate((TAIL - 1, THIN_TAIL - 1)):
        style.reference(ax, guide, anchor * (guide / LAGS[start]) ** -exponent,
                        f"lag^-{exponent:g}", dashed=bool(dashed))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_ylim(bottom=1e-3)
    style.finish(fig, ax, path, end_labels=False)


def plot_impact(summaries: dict, path: Path) -> None:
    fig, ax = style.figure(
        "How far a parent order moves the price, by its size",
        "parent order (lots)",
        "mid move its way, start to end (ticks)",
    )
    for slot, (label, summary) in enumerate(summaries.items()):
        bins = summary["bins"].filter(pl.col("impact") > 0)
        style.line(ax, bins["size"].to_numpy(), bins["impact"].to_numpy(), slot, label,
                   error=bins["error"].to_numpy())
    first = next(iter(summaries.values()))["bins"]
    sizes = first["size"].to_numpy()
    for dashed, exponent in enumerate((1.0, 0.5)):
        style.reference(ax, sizes, first["impact"][0] * (sizes / sizes[0]) ** exponent,
                        f"size^{exponent:g}", dashed=bool(dashed))
    ax.set_xscale("log")
    ax.set_yscale("log")
    style.finish(fig, ax, path, end_labels=False)


def print_tables(signs: dict, memory: dict) -> None:
    print("| market | brokers' share of market orders | orders | "
          + " | ".join(f"lag {lag}" for lag in SHOWN_LAGS)
          + " | fitted exponent | predicted (tail − 1) |")
    print("|---|---:|---|" + "---:|" * len(SHOWN_LAGS) + "---:|---:|")
    for label, summary in signs.items():
        tail = THIN_TAIL if str(THIN_TAIL) in label else TAIL
        for key, who in (("acf_brokers", "brokers'"), ("acf_all", "all")):
            mean, error = summary[key]
            cells = [f"{mean[LAGS == lag][0]:+.4f} ± {error[LAGS == lag][0]:.4f}"
                     for lag in SHOWN_LAGS]
            exponent, exponent_error = summary[f"{key}_exponent"]
            low, high = summary[f"{key}_range"]
            share, share_error = summary["broker_share"]
            print(f"| {label} | {share:.2f} ± {share_error:.2f} | {who} | " + " | ".join(cells)
                  + f" | {exponent:.2f} ± {exponent_error:.2f} (lags {low}–{high}) "
                  f"| {tail - 1:g} |")

    print()
    print("| market | parents | "
          + " | ".join(f"{low}–{high} lots" for low, high in SHOWN_SIZES)
          + " | fitted exponent |")
    print("|---|---:|" + "---:|" * len(SHOWN_SIZES) + "---:|")
    for label, summary in memory.items():
        parents = summary["pooled"]
        cells = []
        for low, high in SHOWN_SIZES:
            chosen = parents.filter(pl.col("lots").is_between(low, high, closed="left"))
            impact = chosen["impact"]
            cells.append(f"{impact.mean():+.2f} ± {impact.std() / np.sqrt(len(impact)):.2f} "
                         f"({len(impact):,})")
        exponent, error = summary["exponent"]
        print(f"| {label} | {parents.height:,} | " + " | ".join(cells)
              + f" | {exponent:.2f} ± {error:.2f} |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="path to the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=16, help="runs of each market")
    parser.add_argument("--duration", default="3600s", help="simulated time of each run")
    parser.add_argument("--first-seed", type=int, default=1)
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "images")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)
    seeds = range(args.first_seed, args.first_seed + args.seeds)
    text = MARKET.read_text()

    variants = {**SIGN_VARIANTS, **{k: v for k, v in MEMORY_VARIANTS.items() if v}}
    jobs = [Run(with_settings(text, changes), seed, args.duration)
            for changes in variants.values() for seed in seeds]
    runs = measure_all(tool, jobs)
    per_variant = {
        label: runs[index * len(seeds) : (index + 1) * len(seeds)]
        for index, label in enumerate(variants)
    }
    per_variant[next(iter(MEMORY_VARIANTS))] = per_variant[next(iter(SIGN_VARIANTS))]

    signs = {label: summarize_signs(per_variant[label]) for label in SIGN_VARIANTS}
    memory = {label: summarize_impact(per_variant[label]) for label in MEMORY_VARIANTS}
    plot_signs(signs, args.out / "sign_memory.png")
    plot_impact(memory, args.out / "parent_impact.png")
    print_tables(signs, memory)


if __name__ == "__main__":
    main()
