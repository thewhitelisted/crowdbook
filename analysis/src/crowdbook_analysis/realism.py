"""Real prediction markets against simulated ones: the same statistics measured on Polymarket's
Bitcoin up-or-down markets and on simulated prediction markets, as a scorecard.

    uv run --project analysis crowdbook-polymarket --from 2026-09-28 --to 2026-10-12
    uv run --project analysis crowdbook-realism

A real market here is one window, such as 9:00 to 9:15 PM: the question is whether Bitcoin ends
it higher than it began, so it opens with nothing known and the price a coin flip, and resolves
when the window closes. A simulated one is a run of a prediction scenario. Both are reduced to
the same record: each aggressive order's time into the window, price as the yes share's in cents,
size and side, and how the question resolved. The chain records only trades, not the book, so
every statistic is one that trades alone give, and times are measured as a share of the window
so that windows of five minutes, fifteen and an hour can be set side by side.

The statistics:

- **Calibration**: at each twentieth of the window, the last price against how the question
  resolved; the Brier score, and how often prices of 10 to 20 and 80 to 90 resolved yes.
- **Moves toward resolution**: the root mean square of price changes over each fifteenth of
  the window, by tenth, for questions still close (20 to 80) and nearly decided.
- **Tails**: the excess kurtosis of those changes while the question is still close.
- **Activity**: the share of aggressive orders in each tenth of the window.
- **Order sizes**: the size of the largest tenth of orders against the median, and the Hill
  exponent of their tail.
- **Order signs**: the autocorrelation of aggressive orders' signs one and ten orders apart.
- **Bounce**: the autocorrelation of successive changes in trade price, which a spread makes
  negative, and the spread it implies (Roll, 1984).
- **The close**: how often the last price before resolution is below 10 or above 90.
- **The value**, where there is one: how its own moves go through the window; how far the price
  is from it; and how late the price is, the delay at which it is closest to the value as it was.
  A simulation knows its value exactly; a real question's is worked out from Bitcoin's price (see
  `bitcoin`), which `--no-bitcoin` leaves out.

The simulated markets are the prediction example and the market fitted by `crowdbook-fit`, under
400 seeds each, none of them seeds the fit used. The charts go to docs/images; the numbers are
printed as Markdown.
"""

import argparse
import datetime as dt
import math
import os
import tempfile
import tomllib
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import bitcoin, facts, polymarket, style
from crowdbook_analysis.log import read_log, read_prices, trades
from crowdbook_analysis.runner import REPO, Run, find_tool, run

MARKET = REPO / "examples" / "scenarios" / "prediction.toml"
SEEDS = 400
# Simulated markets are scored on seeds from here, which crowdbook-fit never fits on.
FIRST_SEED = 100_001
MOMENTS = 20  # moments per window set against the outcome
PARTS = 15  # changes per window measured for their size
TENTHS = 10
LAGS = (1, 10)
MAX_DELAY = 120  # seconds: the longest delay of the price behind the value looked for
TAIL = 0.1  # the share of orders counted as the tail of sizes


@dataclass
class Window:
    """One question, from the window's start to its resolution: each aggressive order's time as
    a share of the window, its price in cents of the yes share, its size and its sign (+1 to
    buy yes), and whether yes won."""

    length: float  # seconds
    time: np.ndarray
    price: np.ndarray
    size: np.ndarray
    sign: np.ndarray
    yes: float
    opening: float = 50.0  # the price as the window opened
    # The true value in cents at each second of the window, where it is known: exactly in a
    # simulation, from Bitcoin's price for a real up-or-down question.
    value: np.ndarray | None = None
    fees: float = 0.0  # cents the orders paid in taker fees


def real_windows(series: str, data: Path = polymarket.DATA, *,
                 with_value: bool = False) -> list[Window]:
    """The windows of a Polymarket series read by `crowdbook-polymarket`, with the orders
    during each one and the last price before it opened, and optionally the probability that
    Bitcoin ends up from its price each second. Windows without a trade are left out."""
    markets, every = polymarket.load(series, data)
    if markets.is_empty():
        return []
    prices = None
    if with_value:
        first = dt.datetime.fromtimestamp(markets["start"].min() - bitcoin.VOLATILITY_WINDOW,
                                          dt.UTC).date()
        last = dt.datetime.fromtimestamp(markets["end"].max(), dt.UTC).date()
        prices = bitcoin.Prices(first, last)
    grouped = {key[0]: frame for key, frame in every.partition_by("slug", as_dict=True).items()}
    windows = []
    for market in markets.iter_rows(named=True):
        orders = grouped.get(market["slug"])
        if orders is None or not 0.0 <= market["yes_payout"] <= 1.0:
            continue
        before = orders.filter(pl.col("time") < market["start"])
        during = orders.filter((pl.col("time") >= market["start"])
                               & (pl.col("time") < market["end"]))
        if during.is_empty():
            continue
        length = float(market["end"] - market["start"])
        windows.append(Window(
            length=length,
            time=(during["time"].to_numpy() - market["start"]) / length,
            price=during["price"].to_numpy(),
            size=during["shares"].to_numpy(),
            sign=np.where(during["side"].to_numpy() == "buy", 1.0, -1.0),
            yes=float(market["yes_payout"]),
            opening=float(before["price"][-1]) if len(before) else 50.0,
            fees=100.0 * float(during["fee"].sum()),
            value=(bitcoin.up_probability(prices, market["start"], market["end"])
                   if prices is not None else None)))
    return windows


def orders_from_trades(prints: pl.DataFrame) -> pl.DataFrame:
    """Aggressive orders from a simulated log's trade prints: the prints one order makes as it
    sweeps the book share its time and side, and become one order at their average price."""
    return (prints.with_columns(
                order=((pl.col("time") != pl.col("time").shift(1))
                       | (pl.col("side") != pl.col("side").shift(1))).fill_null(True).cum_sum())
            .group_by("order", maintain_order=True)
            .agg(pl.col("time").first(), pl.col("side").first(),
                 ((pl.col("price") * pl.col("quantity")).sum() / pl.col("quantity").sum())
                 .alias("price"), pl.col("quantity").sum().alias("size")))


def taker_fees(scenario: str):
    """The taker fee a scenario's exchange charges a lot traded at each price, in cents."""
    exchange = tomllib.loads(scenario).get("exchange", {})
    fee = float(exchange.get("taker_fee", 0.0))
    if exchange.get("fee_curve", "flat") == "probability":
        return lambda price: fee * 4.0 * (price / 100.0) * (1.0 - price / 100.0)
    return lambda price: np.full(np.shape(price), fee)


def simulated_window(tool: Path, job: Run, scratch: Path, index: int) -> Window:
    log_path, prices_path = scratch / f"log{index}.csv", scratch / f"prices{index}.csv"
    result = run(tool, job, log=log_path, log_only="trade", prices=prices_path)
    prints = trades(read_log(log_path))
    orders = orders_from_trades(prints)
    value = read_prices(prices_path)["value"].to_numpy()
    log_path.unlink()
    prices_path.unlink()
    length = result["duration_ns"] / 1e9
    # Each print's taker pays the fee at that print's price, as the exchange charges it.
    fee = taker_fees(job.scenario)
    paid = float(np.sum(fee(prints["price"].to_numpy().astype(float))
                        * prints["quantity"].to_numpy()))
    return Window(length=length, time=orders["time"].to_numpy() / 1e9 / length,
                  price=orders["price"].to_numpy().astype(float),
                  size=orders["size"].to_numpy().astype(float),
                  sign=np.where(orders["side"].to_numpy() == "buy", 1.0, -1.0),
                  yes=result["final_value"] / 100.0, value=value[: int(length)], fees=paid)


def simulated_windows(tool: Path, scenario: str, seeds) -> list[Window]:
    with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(
        max_workers=os.cpu_count()
    ) as pool:
        return list(pool.map(
            lambda seed: simulated_window(tool, Run(scenario, seed), Path(scratch), seed), seeds))


def price_at(window: Window, moments: np.ndarray) -> np.ndarray:
    """The last price at or before each moment, a share of the window; the opening price before
    the first order."""
    sampled = facts.sample_last(window.time, window.price, moments)
    return np.where(np.isnan(sampled), window.opening, sampled)


def calibration(windows: list[Window]) -> dict:
    """The price at each twentieth of the window, before its end, binned by tens of cents, with
    how often those questions resolved yes; and the Brier score of every moment."""
    moments = np.arange(1, MOMENTS) / MOMENTS
    said = np.concatenate([price_at(w, moments) / 100.0 for w in windows])
    outcome = np.concatenate([np.full(len(moments), w.yes) for w in windows])
    bins = np.minimum((said * 10).astype(int), 9)
    table = {}
    for b in range(10):
        chosen = bins == b
        if chosen.sum() >= 20:
            table[b] = (float(said[chosen].mean()), float(outcome[chosen].mean()),
                        int(chosen.sum()))
    return {"bins": table, "brier": float(np.mean((said - outcome) ** 2)),
            "moments": len(said), "yes": float(np.mean([w.yes for w in windows]))}


def changes(windows: list[Window]) -> dict:
    """Changes in price over each fifteenth of the window, with the tenth each starts in and
    whether the question was still close (priced 20 to 80) as it started."""
    edges = np.arange(PARTS + 1) / PARTS
    moves, tenth, close = [], [], []
    for w in windows:
        prices = price_at(w, edges)
        moves.append(np.diff(prices))
        tenth.append(np.minimum((edges[:-1] * TENTHS).astype(int), TENTHS - 1))
        close.append((prices[:-1] >= 20) & (prices[:-1] <= 80))
    return {"move": np.concatenate(moves), "tenth": np.concatenate(tenth),
            "close": np.concatenate(close)}


def moves_by_tenth(windows: list[Window]) -> dict:
    """The root mean square of changes over a fifteenth of the window, by the tenth each starts
    in: over all of them, over those starting close and over those starting nearly decided."""
    found = changes(windows)
    out = {}
    for name, chosen in (("all", np.ones_like(found["close"])), ("close", found["close"]),
                         ("decided", ~found["close"])):
        sums = np.bincount(found["tenth"][chosen], found["move"][chosen] ** 2, TENTHS)
        counts = np.bincount(found["tenth"][chosen], minlength=TENTHS)
        out[name] = np.sqrt(sums / np.maximum(counts, 1))
        out[f"{name}_count"] = counts
    return out


def close_kurtosis(windows: list[Window]) -> float:
    found = changes(windows)
    return facts.excess_kurtosis(found["move"][found["close"]])


def activity(windows: list[Window]) -> np.ndarray:
    """The share of all aggressive orders that came in each tenth of the window."""
    counts = np.zeros(TENTHS)
    for w in windows:
        counts += np.bincount(np.minimum((w.time * TENTHS).astype(int), TENTHS - 1),
                              minlength=TENTHS)
    return counts / counts.sum()


def hill(sizes: np.ndarray, share: float = TAIL) -> float:
    """The Hill estimate of the exponent of a power-law tail, from the largest `share` of
    values: a smaller exponent is a fatter tail."""
    ordered = np.sort(sizes)[::-1]
    top = max(int(len(ordered) * share), 2)
    return float(1.0 / np.mean(np.log(ordered[: top - 1] / ordered[top - 1])))


def sizes(windows: list[Window]) -> dict:
    every = np.concatenate([w.size for w in windows])
    every = every[every > 0]
    median = float(np.median(every))
    return {"median": median, "tail": float(np.quantile(every, 1 - TAIL)) / median,
            "exponent": hill(every)}


def sign_memory(windows: list[Window]) -> dict:
    """The autocorrelation of orders' signs at each of `LAGS`: the mean product of signs that
    many orders apart within one window, each window's signs taken from their own mean, over
    their variance."""
    out = {}
    for lag in LAGS:
        products, pairs, squares, count = 0.0, 0, 0.0, 0
        for w in windows:
            centered = w.sign - w.sign.mean()
            squares += float(np.dot(centered, centered))
            count += len(centered)
            if len(centered) > lag:
                products += float(np.dot(centered[:-lag], centered[lag:]))
                pairs += len(centered) - lag
        out[lag] = (products / pairs) / (squares / count) if pairs and squares else float("nan")
    return out


def bounce(windows: list[Window]) -> dict:
    """The correlation of each change in trade price with the next, over windows, while the
    question is still close; and the spread it implies, 2√(−covariance), in cents."""
    first, second = [], []
    for w in windows:
        steps = np.diff(w.price)
        middle = (w.price[1:-1] >= 20) & (w.price[1:-1] <= 80)
        first.append(steps[:-1][middle])
        second.append(steps[1:][middle])
    a, b = np.concatenate(first), np.concatenate(second)
    covariance = float(np.mean((a - a.mean()) * (b - b.mean())))
    return {"correlation": float(np.corrcoef(a, b)[0, 1]),
            "spread": 2.0 * math.sqrt(-covariance) if covariance < 0 else float("nan")}


def closing(windows: list[Window]) -> float:
    """How often the last price before the window closed was below 10 or above 90."""
    last = np.array([w.price[-1] for w in windows])
    return float(np.mean((last < 10) | (last > 90)))


def lateness(windows: list[Window]) -> dict:
    """How far the last price is from the value, in cents, at each second of each window that has
    a value; and how late the price is: the delay at which it is closest, on average in squares,
    to the value as it was that long before. Only seconds while the value is still close (20 to
    80) count, since a question nearly decided leaves little room to be wrong."""
    known = [w for w in windows if w.value is not None]
    if not known:
        return {}
    delays = np.arange(0, MAX_DELAY + 1)
    squares, counts = np.zeros(len(delays)), np.zeros(len(delays))
    distances, brier = [], []
    for w in known:
        seconds = len(w.value)
        price = price_at(w, np.arange(seconds) / w.length)
        close = (w.value >= 20) & (w.value <= 80)
        good = close & ~np.isnan(w.value)
        distances.append(np.abs(price - w.value)[good])
        moments = (np.arange(1, MOMENTS) * seconds) // MOMENTS
        brier.append((w.value[moments] / 100.0 - w.yes) ** 2)
        for index, delay in enumerate(delays[delays < seconds]):
            gap = price[delay:] - w.value[: seconds - delay]
            chosen = good[: seconds - delay] & ~np.isnan(gap)
            squares[index] += float(np.sum(gap[chosen] ** 2))
            counts[index] += int(chosen.sum())
    curve = np.sqrt(squares / np.maximum(counts, 1))
    every_brier = np.concatenate(brier)
    return {"distance": float(np.mean(np.concatenate(distances))),
            "delay": int(delays[np.argmin(curve)]), "curve": curve,
            "value_brier": float(np.nanmean(every_brier))}


def value_paths(windows: list[Window]) -> list[Window]:
    """The value of each window that has one, as if it were traded every second: for measuring
    how the value itself moves, apart from the prices that follow it."""
    paths = []
    for w in windows:
        if w.value is None or np.isnan(w.value).all():
            continue
        seconds = np.arange(len(w.value))
        known = ~np.isnan(w.value)
        paths.append(Window(length=w.length, time=seconds[known] / w.length,
                            price=w.value[known], size=np.ones(known.sum()),
                            sign=np.ones(known.sum()), yes=w.yes, opening=50.0))
    return paths


def scorecard(windows: list[Window]) -> dict:
    table = calibration(windows)
    orders = np.array([len(w.time) for w in windows])
    lengths = np.array([w.length for w in windows])
    return {
        "windows": len(windows),
        "yes": table["yes"],
        "brier": table["brier"],
        "low": table["bins"].get(1, (math.nan, math.nan, 0))[1],
        "high": table["bins"].get(8, (math.nan, math.nan, 0))[1],
        "calibration": table,
        "orders_per_minute": float(np.mean(orders / lengths * 60.0)),
        "moves": moves_by_tenth(windows),
        "kurtosis": close_kurtosis(windows),
        "activity": activity(windows),
        "sizes": sizes(windows),
        "signs": sign_memory(windows),
        "bounce": bounce(windows),
        "closing": closing(windows),
        "lateness": lateness(windows),
        "value": _value_card(value_paths(windows)),
        "fee_per_share": fee_per_share(windows),
    }


def fee_per_share(windows: list[Window]) -> float:
    """Taker fees in cents for each share or lot the aggressive orders took."""
    shares = sum(float(w.size.sum()) for w in windows)
    return sum(w.fees for w in windows) / shares if shares else float("nan")


def _value_card(paths: list[Window]) -> dict:
    if not paths:
        return {}
    moves = moves_by_tenth(paths)
    return {"first": float(moves["close"][0]), "last": float(moves["close"][-1]),
            "kurtosis": close_kurtosis(paths), "closing": closing(paths)}


ROWS = [
    ("questions", lambda s: f"{s['windows']}"),
    ("resolved yes", lambda s: f"{s['yes']:.1%}"),
    ("Brier score of the price", lambda s: f"{s['brier']:.3f}"),
    ("priced 10 to 20: resolved yes", lambda s: f"{s['low']:.1%}"),
    ("priced 80 to 90: resolved yes", lambda s: f"{s['high']:.1%}"),
    ("aggressive orders per minute", lambda s: f"{s['orders_per_minute']:.0f}"),
    ("moves in the first tenth, close (cents)", lambda s: f"{s['moves']['close'][0]:.1f}"),
    ("moves in the last tenth, close (cents)", lambda s: f"{s['moves']['close'][-1]:.1f}"),
    ("excess kurtosis of moves, close", lambda s: f"{s['kurtosis']:.1f}"),
    ("orders in the first tenth", lambda s: f"{s['activity'][0]:.1%}"),
    ("orders in the last tenth", lambda s: f"{s['activity'][-1]:.1%}"),
    ("largest tenth of orders against the median", lambda s: f"{s['sizes']['tail']:.1f}"),
    ("tail exponent of order sizes", lambda s: f"{s['sizes']['exponent']:.2f}"),
    ("order signs, one apart", lambda s: f"{s['signs'][1]:+.2f}"),
    ("order signs, ten apart", lambda s: f"{s['signs'][10]:+.2f}"),
    ("successive price changes", lambda s: f"{s['bounce']['correlation']:+.2f}"),
    ("spread implied (cents)", lambda s: f"{s['bounce']['spread']:.1f}"),
    ("last price below 10 or above 90", lambda s: f"{s['closing']:.1%}"),
    ("taker fees (cents a share)", lambda s: f"{s['fee_per_share']:.2f}"),
    ("value's moves in the first tenth, close (cents)",
     lambda s: f"{s['value']['first']:.1f}" if s["value"] else "—"),
    ("value's moves in the last tenth, close (cents)",
     lambda s: f"{s['value']['last']:.1f}" if s["value"] else "—"),
    ("excess kurtosis of the value's moves, close",
     lambda s: f"{s['value']['kurtosis']:.1f}" if s["value"] else "—"),
    ("Brier score of the value", lambda s: _known(s, "value_brier", "{:.3f}")),
    ("distance from the value, close (cents)", lambda s: _known(s, "distance", "{:.1f}")),
    ("price closest to the value as it was (seconds before)",
     lambda s: _known(s, "delay", "{}")),
]


def _known(card: dict, key: str, form: str) -> str:
    found = card["lateness"].get(key)
    return "—" if found is None else form.format(found)


def print_scorecard(columns: dict[str, dict]) -> None:
    print("| statistic | " + " | ".join(columns) + " |")
    print("|---|" + "---:|" * len(columns))
    for name, cell in ROWS:
        print(f"| {name} | " + " | ".join(cell(card) for card in columns.values()) + " |")


def print_calibration(columns: dict[str, dict]) -> None:
    print()
    print("| price | " + " | ".join(f"{name}: resolved yes" for name in columns) + " |")
    print("|---|" + "---:|" * len(columns))
    for b in range(10):
        cells = []
        for card in columns.values():
            found = card["calibration"]["bins"].get(b)
            cells.append(f"{found[1]:.1%} ({found[2]})" if found else "—")
        print(f"| {b * 10} to {b * 10 + 10} | " + " | ".join(cells) + " |")


def plot(columns: dict[str, dict], images: Path) -> None:
    fig, ax = style.figure("Real and simulated prices as probabilities",
                           "price, in cents", "resolved yes, %")
    style.reference(ax, [0, 100], [0, 100], "perfectly calibrated", dashed=True)
    for slot, (name, card) in enumerate(columns.items()):
        rows = [card["calibration"]["bins"][b] for b in sorted(card["calibration"]["bins"])]
        style.line(ax, [r[0] * 100 for r in rows], [r[1] * 100 for r in rows], slot, name)
    style.finish(fig, ax, images / "realism_calibration.png", end_labels=False)

    tenths = (np.arange(TENTHS) + 0.5) / TENTHS
    fig, ax = style.figure("Moves while the question is still close, through the window",
                           "share of the window elapsed", "cents per fifteenth, root mean square")
    for slot, (name, card) in enumerate(columns.items()):
        enough = card["moves"]["close_count"] >= 30
        style.line(ax, tenths[enough], card["moves"]["close"][enough], slot, name)
    style.finish(fig, ax, images / "realism_moves.png", end_labels=False)

    fig, ax = style.figure("When aggressive orders come", "share of the window elapsed",
                           "share of orders, %")
    for slot, (name, card) in enumerate(columns.items()):
        style.line(ax, tenths, card["activity"] * 100, slot, name)
    style.finish(fig, ax, images / "realism_activity.png", end_labels=False)

    fig, ax = style.figure("How late the price is: its distance from the value as it was",
                           "seconds before", "cents, root mean square")
    for slot, (name, card) in enumerate(columns.items()):
        if card["lateness"]:
            curve = card["lateness"]["curve"][:61]
            style.line(ax, np.arange(len(curve)), curve, slot, name, markers=False)
    style.finish(fig, ax, images / "realism_lateness.png", end_labels=False)


NAMES = {"btc-up-or-down-5m": "real, 5 min", "btc-up-or-down-15m": "real, 15 min",
         "btc-up-or-down-hourly": "real, hourly"}
FITTED = REPO / "examples" / "scenarios" / "up_or_down.toml"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=SEEDS)
    parser.add_argument("--scenario", type=Path, nargs="+", default=[MARKET, FITTED],
                        help="simulated markets to set against the real ones")
    parser.add_argument("--series", nargs="+", default=list(polymarket.SERIES))
    parser.add_argument("--data", type=Path, default=polymarket.DATA)
    parser.add_argument("--images", type=Path, default=REPO / "docs" / "images")
    parser.add_argument("--no-bitcoin", dest="bitcoin", action="store_false",
                        help="leave out the value worked out from Bitcoin's price")
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)

    columns = {}
    for series in args.series:
        windows = real_windows(series, args.data, with_value=args.bitcoin)
        if windows:
            columns[NAMES.get(series, series)] = scorecard(windows)
    for path in args.scenario:
        if path.exists():
            simulated = simulated_windows(tool, path.read_text(),
                                          range(FIRST_SEED, FIRST_SEED + args.seeds))
            columns[f"simulated: {path.stem}"] = scorecard(simulated)
    print_scorecard(columns)
    print_calibration(columns)
    # Four series at most to a chart: the five-minute questions, whose value is the roughest
    # stand-in, are left to the tables.
    plotted = {name: card for name, card in columns.items() if name != NAMES[polymarket.SERIES[0]]}
    plot(dict(list(plotted.items())[: len(style.SERIES)]), args.images)


if __name__ == "__main__":
    main()
