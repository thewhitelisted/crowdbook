"""Measures stylized facts of the large example markets and plots them.

    uv run --project analysis crowdbook-facts

Three sets of runs, all through crowdbook:

- examples/scenarios/large_market.toml, large_noise.toml and memory_market.toml for a simulated
  day under each of several seeds, with prices sampled every second: returns over horizons from
  a second to five minutes, their tails and autocorrelation, the volatility measured at each
  horizon, and how long volatility clusters;
- an hour of each under the same seeds with its event log: the spread, and how the mid moves
  after the aggressive orders of each kind of trader;
- the mixed market with one group of agents left out at a time, to see which of them shape it;
- the memory market with one of its sources of memory taken away at a time, or adaptive traders
  added, to see what makes volatility cluster;
- the noise traders alone as 10, 100 or 1,000 agents sending the same total order flow, to see
  whether the number of agents matters.

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
from crowdbook_analysis.log import aggressive_orders, quotes, read_log, read_prices
from crowdbook_analysis.runner import REPO, Run, agent_groups, find_tool, pnl_at_value, run

MARKETS = {
    "mixed market": REPO / "examples" / "scenarios" / "large_market.toml",
    "noise traders only": REPO / "examples" / "scenarios" / "large_noise.toml",
    "memory market": REPO / "examples" / "scenarios" / "memory_market.toml",
}
HORIZONS = {"1 s": 1, "10 s": 10, "1 min": 60, "5 min": 300}  # in one-second samples
SIGNATURE_HORIZONS = [1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600]  # seconds
TIME_TICKS = {1: "1 s", 10: "10 s", 60: "1 min", 600: "10 min", 3600: "1 h"}
TAIL_HORIZONS = ["10 s", "1 min"]
ACF_HORIZON = "10 s"  # where the trend followers leave their mark
MAX_LAG = 30
CLUSTERING_LAGS = 120  # minutes, for the absolute values of one-minute returns
CLUSTERING_TICKS = {1: "1 min", 10: "10 min", 60: "1 h", 120: "2 h"}

IMPACT_SECONDS = [0.001, 0.003, 0.01, 0.03, 0.1, 0.3, 1, 3, 10, 30, 60, 120]
IMPACT_TICKS = {0.001: "1 ms", 0.01: "10 ms", 0.1: "100 ms", 1: "1 s", 10: "10 s", 60: "1 min"}
# The trader groups of large_market.toml whose aggressive orders are followed.
IMPACT_GROUPS = ["noise", "trend", "experts", "followers"]
# Groups left out together, to see what each kind of trader does to the market.
LEFT_OUT = {("maker", "slow maker"): "without the market makers",
            ("trend",): "without the trend followers",
            ("experts", "followers"): "without the traders who follow the value"}

# The noise traders of large_noise.toml divided among more or fewer agents, each sending orders
# proportionally faster, so that the market as a whole sees the same flow. Positions are
# unlimited, so no agent is held back by its limit however much it trades.
CROWD = """
reference_price = 10000

[[agents]]
type = "zero_intelligence"
name = "noise"
count = {count}
limit_rate = {limit_rate}
market_rate = {market_rate}
cancel_rate = 0.2
max_offset = 10
latency = {{ to_exchange = "300us", from_exchange = "300us", jitter = "100us" }}
account = {{ max_position = 1000000000, max_order_quantity = 10 }}
"""
CROWD_SIZES = [10, 100, 1000]
CROWD_LIMIT_RATE = 400.0  # limit orders per second from the whole crowd
CROWD_MARKET_RATE = 100.0  # market orders per second from the whole crowd


# Fifty traders who switch between value and trend strategies, added to the memory market.
ADAPTIVE = """
[[agents]]
type = "adaptive"
name = "adaptive"
count = 50
interval = "1s"
noise = 2.0
fast_half_life = "5s"
slow_half_life = "60s"
memory = "600s"
choice_intensity = 0.5
threshold = 1.0
order_size = 5
max_position = 200
latency = { to_exchange = "500us", from_exchange = "500us", jitter = "200us" }
account = { max_position = 200, max_order_quantity = 5 }
"""


def without_keys(text: str, keys: list[str]) -> str:
    """The scenario with every setting called one of `keys` left out, so it takes its default."""
    lines = text.splitlines(keepends=True)
    kept = [line for line in lines if line.split("=")[0].strip() not in keys]
    if len(lines) - len(kept) < len(keys):
        raise ValueError(f"the scenario does not set all of {keys}")
    return "".join(kept)


def with_group_setting(text: str, name: str, key: str, value: str) -> str:
    """The scenario with one setting of the agent group called `name` changed."""
    head, *groups = re.split(r"(?m)^(?=\[\[agents\]\])", text)
    changed = 0
    for index, group in enumerate(groups):
        if f'name = "{name}"' in group:
            groups[index], count = re.subn(rf"(?m)^{key} = [^#\n]*?(\s*#.*)?$",
                                           f"{key} = {value}", group)
            changed += count
    if changed != 1:
        raise ValueError(f"the scenario does not set {key} once for the group {name}")
    return head + "".join(groups)


MEMORY_VARIANTS = {
    "without news": lambda text: without_keys(text, ["jump_rate", "jump_size"]),
    "without the activity response": lambda text: without_keys(text, ["activity_response"]),
    "without the volatility response": lambda text: without_keys(text, ["volatility_response"]),
    "with followers who act 2.5 ticks off": lambda text: with_group_setting(
        text, "followers", "threshold", "2.5"),
    "with fifty adaptive traders": lambda text: text + ADAPTIVE,
}


def without_group(text: str, names: tuple[str, ...]) -> str:
    """The scenario with its agent groups of these names left out."""
    head, *groups = re.split(r"(?m)^(?=\[\[agents\]\])", text)
    kept = [group for group in groups
            if not any(f'name = "{name}"' in group for name in names)]
    if len(kept) != len(groups) - len(names):
        raise ValueError(f"the scenario does not have one agent group for each of {names}")
    return head + "".join(kept)


def price_statistics(prices: pl.DataFrame) -> dict:
    """Statistics of one run's prices, sampled every second."""
    mids = prices["mid"].to_numpy()
    horizons = {}
    for label, steps in HORIZONS.items():
        returns = facts.horizon_returns(mids, steps)
        horizons[label] = {
            "returns": returns,
            "kurtosis": facts.excess_kurtosis(returns),
            "acf": facts.autocorrelation(returns, MAX_LAG),
            "acf_abs": facts.autocorrelation(np.abs(returns), MAX_LAG),
        }
    spreads = (prices["ask"] - prices["bid"]).drop_nulls().to_numpy()
    minutes = np.abs(facts.horizon_returns(mids, 60))
    return {
        "horizons": horizons,
        "clustering": facts.autocorrelation(minutes, CLUSTERING_LAGS),
        "signature": [
            facts.volatility_per_root_second(mids, steps, 1.0) for steps in SIGNATURE_HORIZONS
        ],
        "mean_spread": float(spreads.mean()),
    }


def run_statistics(result: dict) -> dict:
    """Trading activity and each group's PnL per lot traded, with positions valued at the true
    value when there is one."""
    seconds = result["duration_ns"] / 1e9
    value = result["final_value"] if result["final_value"] is not None else result["last_price"]
    return {
        "trades_per_second": result["trades"] / seconds,
        "lots_per_second": {each["name"]: each["traded"] / seconds for each in result["groups"]},
        "pnl_per_lot": {
            each["name"]: pnl_at_value(each, value) / each["traded"] if each["traded"] else np.nan
            for each in result["groups"]
        },
    }


def measure_prices(tool: Path, jobs: list[Run], scratch: Path) -> list[dict]:
    """Runs the jobs in parallel, each sampling its prices every second, and returns the
    statistics of each one's prices."""

    def measure(index: int, job: Run) -> dict:
        path = scratch / f"prices-{index}.csv"
        result = run(tool, job, prices=path)
        statistics = price_statistics(read_prices(path))
        path.unlink()
        return statistics | run_statistics(result)

    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        return list(pool.map(measure, range(len(jobs)), jobs))


def summarize(runs: list[dict]) -> dict:
    """The mean and standard error over runs of each statistic, with the one-minute returns of
    all the runs together."""
    horizons = {}
    for label in HORIZONS:
        each = [one["horizons"][label] for one in runs]
        horizons[label] = {
            "returns": sum(len(one["returns"]) for one in each),
            "kurtosis": facts.mean_and_error([one["kurtosis"] for one in each]),
            "acf": facts.mean_and_error([one["acf"] for one in each]),
            "acf_abs": facts.mean_and_error([one["acf_abs"] for one in each]),
        }
    return {
        "horizons": horizons,
        "pooled_returns": {
            label: np.concatenate([one["horizons"][label]["returns"] for one in runs])
            for label in TAIL_HORIZONS
        },
        "signature": facts.mean_and_error([one["signature"] for one in runs]),
        "clustering": facts.mean_and_error([one["clustering"] for one in runs]),
        "mean_spread": facts.mean_and_error([one["mean_spread"] for one in runs]),
        "trades_per_second": facts.mean_and_error([one["trades_per_second"] for one in runs]),
        "lots_per_second": {
            name: facts.mean_and_error([one["lots_per_second"][name] for one in runs])
            for name in runs[0]["lots_per_second"]
        },
        "pnl_per_lot": {
            name: facts.mean_and_error([one["pnl_per_lot"][name] for one in runs])
            for name in runs[0]["pnl_per_lot"]
        },
    }


def measure_log(tool: Path, job: Run, scratch: Path) -> dict:
    """Runs the job with its event log and measures the spread and each group's price impact."""
    path = scratch / "log.csv"
    result = run(tool, job, log=path, log_only="trade,top_of_book,filled")
    log = read_log(path)
    path.unlink()
    end = result["duration_ns"]
    book = quotes(log)
    orders = aggressive_orders(log).with_columns(
        group=pl.col("agent").replace_strict(agent_groups(result), return_dtype=pl.Utf8)
    )
    horizons = [round(seconds * 1e9) for seconds in IMPACT_SECONDS]
    return {
        "spreads": facts.spread_shares(book, end),
        "impact": facts.impact_response(orders, book, horizons, "group", end),
    }


def combine_logs(runs: list[dict]) -> dict:
    """The mean over runs of the spread distribution and of each group's price impact. Impact's
    standard error is taken across runs, because the orders of one run share its price path and
    are far from independent at long horizons."""
    spreads = sorted({spread for one in runs for spread in one["spreads"]})
    impact = (
        pl.concat(one["impact"] for one in runs)
        .group_by("group", "horizon_ns")
        .agg(
            pl.col("move").mean(),
            (pl.col("move").std() / pl.len().sqrt()).alias("error"),
            pl.col("orders").sum(),
        )
        .sort("group", "horizon_ns")
    )
    return {
        "spreads": {
            spread: float(np.mean([one["spreads"].get(spread, 0.0) for one in runs]))
            for spread in spreads
        },
        "impact": impact,
    }


def time_axis(ax, ticks: dict) -> None:
    ax.set_xscale("log")
    ax.set_xticks(list(ticks), labels=list(ticks.values()))
    ax.minorticks_off()


def plot_tails(markets: dict, path: Path) -> None:
    fig, axes = style.panels(
        "Tails of returns",
        [f"{label} returns" for label in TAIL_HORIZONS],
        "|return| in standard deviations",
        "share of returns at least this large",
    )
    for ax, horizon in zip(axes, TAIL_HORIZONS, strict=True):
        largest = 0.0
        fewest = min(len(one["pooled_returns"][horizon]) for one in markets.values())
        for slot, (label, summary) in enumerate(markets.items()):
            z, share = facts.tail_distribution(summary["pooled_returns"][horizon])
            style.line(ax, z, share, slot, label, markers=False)
            largest = max(largest, z[-1])
        grid = np.linspace(0.0, largest, 200)
        normal = facts.normal_tail(grid)
        shown = normal >= 1.0 / fewest
        style.reference(ax, grid[shown], normal[shown], "normal distribution")
        ax.set_yscale("log")
    axes[0].set_ylim(bottom=0.5 / max(len(one["pooled_returns"][TAIL_HORIZONS[0]])
                                      for one in markets.values()))
    style.finish(fig, axes, path, end_labels=False)


def plot_autocorrelation(markets: dict, path: Path) -> None:
    step = HORIZONS[ACF_HORIZON]
    fig, axes = style.panels(
        f"Autocorrelation of {ACF_HORIZON} returns",
        ["returns", "absolute returns (volatility clustering)"],
        "lag (seconds)",
        "autocorrelation",
    )
    lags = np.arange(1, MAX_LAG + 1) * step
    samples = min(one["horizons"][ACF_HORIZON]["returns"] for one in markets.values())
    noise = 1.96 / np.sqrt(samples)
    for ax, key in zip(axes, ("acf", "acf_abs"), strict=True):
        style.band(ax, -noise, noise, "95% band for no correlation")
        ax.axhline(0, color=style.AXIS, linewidth=style.HAIRLINE)
        for slot, (label, summary) in enumerate(markets.items()):
            mean, _ = summary["horizons"][ACF_HORIZON][key]
            style.line(ax, lags, mean, slot, label, markers=False)
    style.finish(fig, axes, path, end_labels=False)


def plot_clustering(markets: dict, path: Path) -> None:
    fig, ax = style.figure(
        "How long volatility clusters",
        "lag between one-minute returns",
        "autocorrelation of their absolute values",
    )
    lags = np.arange(1, CLUSTERING_LAGS + 1)
    samples = min(one["horizons"]["1 min"]["returns"] for one in markets.values())
    noise = 1.96 / np.sqrt(samples)
    style.band(ax, -noise, noise, "95% band for no correlation")
    ax.axhline(0, color=style.AXIS, linewidth=style.HAIRLINE)
    for slot, (label, summary) in enumerate(markets.items()):
        mean, _ = summary["clustering"]
        style.line(ax, lags, mean, slot, label, markers=False)
    time_axis(ax, CLUSTERING_TICKS)
    style.finish(fig, ax, path, end_labels=False)


def plot_signature(curves: dict, title: str, path: Path) -> None:
    fig, ax = style.figure(title, "horizon", "volatility per √second (ticks)")
    for slot, (label, (mean, error)) in enumerate(curves.items()):
        style.line(ax, SIGNATURE_HORIZONS, mean, slot, label, error=error)
    time_axis(ax, TIME_TICKS)
    ax.set_ylim(bottom=0)
    style.finish(fig, ax, path)


def plot_spreads(logged: dict, path: Path) -> None:
    fig, ax = style.figure(
        "How wide the spread was", "spread (ticks)", "share of time with both sides quoted"
    )
    widest = max(
        spread
        for one in logged.values()
        for spread, share in one["spreads"].items()
        if share >= 0.001
    )
    offsets = np.linspace(-0.12, 0.12, len(logged))
    for slot, (label, one) in enumerate(logged.items()):
        shown = {spread: share for spread, share in one["spreads"].items() if spread <= widest}
        style.stems(ax, list(shown), list(shown.values()), slot, label, offset=offsets[slot])
    ax.set_ylim(bottom=0)
    ax.set_xticks(range(1, widest + 1))
    style.finish(fig, ax, path, end_labels=False)


def plot_impact(impact: pl.DataFrame, path: Path) -> None:
    fig, ax = style.figure(
        "How the price moves after each kind of trader's aggressive orders",
        "time after the order",
        "mid move toward the order (ticks)",
    )
    ax.axhline(0, color=style.AXIS, linewidth=style.HAIRLINE)
    for slot, name in enumerate(IMPACT_GROUPS):
        rows = impact.filter(pl.col("group") == name)
        seconds = rows["horizon_ns"].to_numpy() / 1e9
        style.line(
            ax, seconds, rows["move"].to_numpy(), slot, name, error=rows["error"].to_numpy()
        )
    time_axis(ax, IMPACT_TICKS)
    style.finish(fig, ax, path)


def estimate(pair: tuple, digits: int = 2, signed: bool = True) -> str:
    mean, error = pair
    return f"{mean:{'+' if signed else ''}.{digits}f} ± {error:.{digits}f}"


def print_tables(markets: dict, variants: dict, memory: dict, logged: dict,
                 crowds: dict) -> None:
    print("| market | horizon | returns | excess kurtosis | lag-1 autocorrelation | "
          "lag-1 autocorrelation of absolute returns |")
    print("|---|---|---:|---:|---:|---:|")
    for label, summary in markets.items():
        for horizon, stats in summary["horizons"].items():
            acf, acf_error = stats["acf"]
            acf_abs, acf_abs_error = stats["acf_abs"]
            print(f"| {label} | {horizon} | {stats['returns']:,} | "
                  f"{estimate(stats['kurtosis'])} | {acf[0]:+.3f} ± {acf_error[0]:.3f} | "
                  f"{acf_abs[0]:+.3f} ± {acf_abs_error[0]:.3f} |")

    shown = [SIGNATURE_HORIZONS.index(seconds) for seconds in TIME_TICKS]
    print()
    print("| market | trades per second | mean spread (ticks) | "
          + " | ".join(f"volatility at {label}" for label in TIME_TICKS.values()) + " |")
    print("|---|---:|---:|" + "---:|" * len(shown))
    for label, summary in markets.items():
        mean, error = summary["signature"]
        print(f"| {label} | {estimate(summary['trades_per_second'], 0, signed=False)} | "
              f"{estimate(summary['mean_spread'], signed=False)} | "
              + " | ".join(f"{mean[i]:.2f} ± {error[i]:.2f}" for i in shown) + " |")

    print()
    print("| market | excess kurtosis at 10 s | excess kurtosis at 1 min | "
          "absolute-return autocorrelation at 10 s | absolute-return autocorrelation at 1 min | "
          "volatility at 1 h |")
    print("|---|---:|---:|---:|---:|---:|")
    hour = SIGNATURE_HORIZONS.index(3600)
    for label, summary in variants.items():
        stats = summary["horizons"]
        cells = [estimate(stats["10 s"]["kurtosis"]), estimate(stats["1 min"]["kurtosis"])]
        for horizon in ("10 s", "1 min"):
            mean, error = stats[horizon]["acf_abs"]
            cells.append(f"{mean[0]:+.3f} ± {error[0]:.3f}")
        mean, error = summary["signature"]
        cells.append(f"{mean[hour]:.2f} ± {error[hour]:.2f}")
        print(f"| {label} | " + " | ".join(cells) + " |")

    print()
    shown_lags = [1, 15, 60, 120]
    print("| market | trades per second | excess kurtosis at 1 min | "
          + " | ".join(f"clustering at {lag} min" for lag in shown_lags)
          + " | volatility at 1 h |")
    print("|---|---:|---:|" + "---:|" * len(shown_lags) + "---:|")
    for label, summary in memory.items():
        mean, error = summary["clustering"]
        signature, signature_error = summary["signature"]
        print(f"| {label} | {estimate(summary['trades_per_second'], 0, signed=False)} | "
              f"{estimate(summary['horizons']['1 min']['kurtosis'])} | "
              + " | ".join(f"{mean[lag - 1]:+.3f} ± {error[lag - 1]:.3f}" for lag in shown_lags)
              + f" | {signature[hour]:.2f} ± {signature_error[hour]:.2f} |")

    mixed = markets["mixed market"]
    print()
    print("| trader | lots traded per second | PnL per lot traded (ticks, at the true value) |")
    print("|---|---:|---:|")
    for name, pnl in mixed["pnl_per_lot"].items():
        print(f"| {name} | {estimate(mixed['lots_per_second'][name], 1, signed=False)} | "
              f"{estimate(pnl, 3)} |")

    impact = logged["mixed market"]["impact"]
    print()
    print("| trader | aggressive orders | "
          + " | ".join(IMPACT_TICKS.values()) + " | 2 min |")
    print("|---|---:|" + "---:|" * (len(IMPACT_TICKS) + 1))
    for name in IMPACT_GROUPS:
        rows = impact.filter(pl.col("group") == name)
        cells = []
        for seconds in [*IMPACT_TICKS, 120]:
            row = rows.filter(pl.col("horizon_ns") == round(seconds * 1e9)).row(0, named=True)
            cells.append(f"{row['move']:+.2f} ± {row['error']:.2f}")
        print(f"| {name} | {rows['orders'].max():,} | " + " | ".join(cells) + " |")

    print()
    print("| noise traders | orders per trader per second | trades per second | "
          "mean spread (ticks) | lag-1 autocorrelation at 1 s | volatility at 1 s | "
          "volatility at 10 min | excess kurtosis at 1 min |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|")
    for count, summary in crowds.items():
        rate = (CROWD_LIMIT_RATE + CROWD_MARKET_RATE) / count
        acf, acf_error = summary["horizons"]["1 s"]["acf"]
        mean, error = summary["signature"]
        one, ten_minutes = SIGNATURE_HORIZONS.index(1), SIGNATURE_HORIZONS.index(600)
        print(f"| {count:,} | {rate:g} | "
              f"{estimate(summary['trades_per_second'], 1, signed=False)} | "
              f"{estimate(summary['mean_spread'], 3, signed=False)} | "
              f"{acf[0]:+.3f} ± {acf_error[0]:.3f} | "
              f"{mean[one]:.3f} ± {error[one]:.3f} | "
              f"{mean[ten_minutes]:.3f} ± {error[ten_minutes]:.3f} | "
              f"{estimate(summary['horizons']['1 min']['kurtosis'])} |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="path to the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=4, help="runs of each market")
    parser.add_argument("--duration", default="86400s", help="simulated time of each run")
    parser.add_argument("--log-duration", default="3600s", help="simulated time of logged runs")
    parser.add_argument("--first-seed", type=int, default=21)
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "images")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)
    seeds = range(args.first_seed, args.first_seed + args.seeds)

    texts = {label: path.read_text() for label, path in MARKETS.items()}
    crowd_texts = {
        count: CROWD.format(
            count=count,
            limit_rate=CROWD_LIMIT_RATE / count,
            market_rate=CROWD_MARKET_RATE / count,
        )
        for count in CROWD_SIZES
    }
    mixed = texts["mixed market"]
    variant_texts = {label: without_group(mixed, names) for names, label in LEFT_OUT.items()}
    memory_texts = {
        label: change(texts["memory market"]) for label, change in MEMORY_VARIANTS.items()
    }
    cases = [*texts.values(), *variant_texts.values(), *memory_texts.values(),
             *crowd_texts.values()]
    jobs = [Run(text, seed, args.duration) for text in cases for seed in seeds]

    with tempfile.TemporaryDirectory() as scratch:
        statistics = measure_prices(tool, jobs, Path(scratch))
        logged = {
            label: combine_logs(
                [
                    measure_log(tool, Run(text, seed, args.log_duration), Path(scratch))
                    for seed in seeds
                ]
            )
            for label, text in texts.items()
        }
    per_case = [
        summarize(statistics[index * len(seeds) : (index + 1) * len(seeds)])
        for index in range(len(cases))
    ]
    remaining = iter(per_case)
    markets = {label: next(remaining) for label in texts}
    left_out = {label: next(remaining) for label in variant_texts}
    memory_variants = {label: next(remaining) for label in memory_texts}
    crowds = {count: next(remaining) for count in crowd_texts}
    variants = {"mixed market": markets["mixed market"], **left_out,
                "noise traders only": markets["noise traders only"]}
    memory = {"memory market": markets["memory market"], **memory_variants,
              "mixed market, without memory": markets["mixed market"]}

    plot_tails(markets, args.out / "return_tails.png")
    plot_autocorrelation(markets, args.out / "autocorrelation.png")
    plot_signature(
        {label: summary["signature"] for label, summary in markets.items()},
        "Volatility measured at different horizons",
        args.out / "volatility_signature.png",
    )
    plot_signature(
        {f"{count:,} noise traders": summary["signature"] for count, summary in crowds.items()},
        "The same order flow from 10, 100 or 1,000 noise traders",
        args.out / "crowd_size.png",
    )
    plot_clustering(markets, args.out / "clustering.png")
    plot_spreads(logged, args.out / "spreads.png")
    plot_impact(logged["mixed market"]["impact"], args.out / "impact.png")
    print_tables(markets, variants, memory, logged, crowds)


if __name__ == "__main__":
    main()
