"""Recorded markets in one format, whether they come from an exchange or from crowdbook: a tape.

A tape is a directory of tables, as CSV or Parquet, that hold one or many markets:

- `markets.csv` (required): one row per market. `market` names it and `duration` is its length in
  seconds; every time in the other tables is seconds from the market's start. `outcome`, if
  given, is what one unit paid when the market settled, in the same units as prices. Any other
  column is a label: for a simulated market, the hidden settings it was made with.
- `book.csv` (required): snapshots of the book, one row each: `market`, `time`, then
  `bid_price_1`, `bid_size_1`, `ask_price_1`, `ask_size_1` and so on for as many levels as were
  recorded. A missing level is empty. Each snapshot holds until the next, so snapshots may come
  at any times, every change or every second.
- `trades.csv` (required): one row per print: `market`, `time`, `price`, `size` and `side`, the
  side of the order that took liquidity (`buy` or `sell`). Where `side` is empty it is inferred
  from the book: above the mid a buy, below a sell, at the mid the same as the last print.
- `value.csv` (optional): `market`, `time` and `value`, a reference value the price can be set
  against, such as a probability worked out from another market or a simulation's true value.
- `scores.csv` (optional, for games): `market`, `time`, `home` and `away`, the score after each
  change, which a calibration can make its synthetic markets replay (see `calibrate`).
- `tape.toml` (optional): `tick`, the price increment (1 if absent); `returns`, how price changes
  are measured, `"difference"` (the default, for prices with bounds such as probabilities) or
  `"log"`; and `payout`, for a binary contract, what a winning unit pays.

Any of the tables may be Parquet instead (`book.parquet` and so on). `read_tape` reads a tape and
`write_tape` writes one; `simulate` runs a crowdbook scenario under many seeds into a tape, so that
simulated markets and recorded ones can be measured by the same code (see `validate`).
"""

import os
import tempfile
import tomllib
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from functools import partial
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis.log import read_log, read_prices, trades
from crowdbook_analysis.runner import Run, pnl_at_value, run

# How often a simulated market's book and value are sampled into its tape, in seconds.
SAMPLE_INTERVAL = 0.1


@dataclass
class Options:
    """What a tape says about its prices."""

    tick: float = 1.0
    returns: str = "difference"  # or "log"
    payout: float | None = None  # what a winning unit of a binary contract pays


@dataclass
class Market:
    """One market's recording, as arrays. Book levels are columns, best first; a missing level is
    NaN. Times are seconds from the market's start."""

    name: str
    duration: float
    book_time: np.ndarray
    bid_price: np.ndarray  # snapshots by levels
    bid_size: np.ndarray
    ask_price: np.ndarray
    ask_size: np.ndarray
    trade_time: np.ndarray
    trade_price: np.ndarray
    trade_size: np.ndarray
    trade_sign: np.ndarray  # +1 where a buyer took liquidity, -1 a seller
    outcome: float | None = None
    value_time: np.ndarray | None = None
    value: np.ndarray | None = None
    labels: dict = field(default_factory=dict)
    # For a game: the score after each change, as rows of time, home and away.
    scores: np.ndarray | None = None


@dataclass
class Tape:
    markets: list[Market]
    options: Options = field(default_factory=Options)


def _table(directory: Path, name: str, required: bool = True) -> pl.DataFrame | None:
    # A CSV's whole length decides each column's type, so that a column whole for a while and
    # then not is read as numbers with fractions.
    csv = partial(pl.read_csv, infer_schema_length=None)
    for suffix, reader in ((".parquet", pl.read_parquet), (".csv", csv)):
        path = directory / f"{name}{suffix}"
        if path.is_file():
            frame = reader(path)
            if "market" in frame.columns:
                frame = frame.with_columns(pl.col("market").cast(pl.Utf8))
            return frame
    if required:
        raise FileNotFoundError(f"{directory} has no {name}.csv or {name}.parquet")
    return None


def _levels(book: pl.DataFrame, side: str) -> int:
    count = 0
    while f"{side}_price_{count + 1}" in book.columns:
        count += 1
    return count


def _matrix(frame: pl.DataFrame, columns: list[str]) -> np.ndarray:
    if not columns:
        return np.full((len(frame), 1), np.nan)
    # One query for all the columns: a query a column costs more than the reading.
    return frame.select([pl.col(c).cast(pl.Float64).fill_null(np.nan) for c in columns]) \
        .to_numpy().astype(float, copy=False).reshape(len(frame), len(columns))


def infer_signs(time: np.ndarray, price: np.ndarray, book_time: np.ndarray,
                bid: np.ndarray, ask: np.ndarray) -> np.ndarray:
    """Which side took liquidity at each print, from the last snapshot before it: above the mid a
    buy, below it a sell, and at the mid, or with no quote, the same as the print before (a buy
    for the first)."""
    before = np.searchsorted(book_time, time, side="left") - 1
    mid = np.full(len(time), np.nan)
    known = before >= 0
    mid[known] = (bid[before[known]] + ask[before[known]]) / 2
    signs = np.sign(price - mid)
    signs[~np.isfinite(signs)] = 0
    last = 1.0
    for i, s in enumerate(signs):
        if s == 0:
            signs[i] = last
        last = signs[i]
    return signs


def read_tape(directory: Path | str) -> Tape:
    """Reads every market of a tape, in the order `markets.csv` lists them."""
    directory = Path(directory)
    settings = {}
    if (directory / "tape.toml").is_file():
        settings = tomllib.loads((directory / "tape.toml").read_text())
    options = Options(tick=float(settings.get("tick", 1.0)),
                      returns=settings.get("returns", "difference"),
                      payout=settings.get("payout"))
    if options.returns not in ("difference", "log"):
        raise ValueError(f"tape.toml: returns is 'difference' or 'log', not {options.returns!r}")
    listed = _table(directory, "markets")
    book = _table(directory, "book").sort("market", "time", maintain_order=True)
    prints = _table(directory, "trades").sort("market", "time", maintain_order=True)
    values = _table(directory, "value", required=False)
    scored = _table(directory, "scores", required=False)
    scored = scored.sort("market", "time").partition_by("market", as_dict=True) \
        if scored is not None else {}
    books = book.partition_by("market", as_dict=True)
    printed = prints.partition_by("market", as_dict=True)
    valued = values.sort("market", "time").partition_by("market", as_dict=True) \
        if values is not None else {}
    bid_levels, ask_levels = _levels(book, "bid"), _levels(book, "ask")
    known = {"market", "duration", "outcome"}
    markets = []
    empty_book = book.clear()
    empty_prints = prints.clear()
    for row in listed.iter_rows(named=True):
        name = str(row["market"])
        snaps = books.get((name,), empty_book)
        tape = printed.get((name,), empty_prints)
        book_time = snaps["time"].cast(pl.Float64).to_numpy()
        bid_price = _matrix(snaps, [f"bid_price_{i}" for i in range(1, bid_levels + 1)])
        ask_price = _matrix(snaps, [f"ask_price_{i}" for i in range(1, ask_levels + 1)])
        trade_time = tape["time"].cast(pl.Float64).to_numpy()
        trade_price = tape["price"].cast(pl.Float64).to_numpy()
        if "side" in tape.columns:
            side = tape["side"].to_numpy()
            signs = np.where(side == "buy", 1.0, np.where(side == "sell", -1.0, 0.0))
        else:
            signs = np.zeros(len(tape))
        if np.any(signs == 0):
            inferred = infer_signs(trade_time, trade_price, book_time, bid_price[:, 0],
                                   ask_price[:, 0])
            signs = np.where(signs == 0, inferred, signs)
        value = valued.get((name,))
        score = scored.get((name,))
        outcome = row.get("outcome")
        markets.append(Market(
            name=name, duration=float(row["duration"]), book_time=book_time,
            bid_price=bid_price,
            bid_size=_matrix(snaps, [f"bid_size_{i}" for i in range(1, bid_levels + 1)]),
            ask_price=ask_price,
            ask_size=_matrix(snaps, [f"ask_size_{i}" for i in range(1, ask_levels + 1)]),
            trade_time=trade_time, trade_price=trade_price,
            trade_size=tape["size"].cast(pl.Float64).to_numpy(), trade_sign=signs,
            outcome=None if outcome is None else float(outcome),
            value_time=None if value is None else value["time"].cast(pl.Float64).to_numpy(),
            value=None if value is None else value["value"].cast(pl.Float64).to_numpy(),
            labels={k: v for k, v in row.items() if k not in known},
            scores=None if score is None else score.select(
                pl.col("time").cast(pl.Float64), pl.col("home").cast(pl.Float64),
                pl.col("away").cast(pl.Float64)).to_numpy()))
    return Tape(markets, options)


def write_tape(tape: Tape, directory: Path | str, *, parquet: bool = False) -> None:
    """Writes a tape, as CSV or Parquet; `read_tape` reads it back as it was."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)

    def save(frame: pl.DataFrame, name: str) -> None:
        if parquet:
            frame.write_parquet(directory / f"{name}.parquet")
        else:
            frame.write_csv(directory / f"{name}.csv")

    rows = []
    for m in tape.markets:
        row = {"market": m.name, "duration": m.duration}
        if m.outcome is not None:
            row["outcome"] = m.outcome
        rows.append(row | m.labels)
    save(pl.DataFrame(rows, infer_schema_length=None), "markets")
    levels = max((m.bid_price.shape[1] for m in tape.markets), default=1)
    books, prints, values, scores = [], [], [], []
    for m in tape.markets:
        columns = {"market": [m.name] * len(m.book_time), "time": m.book_time}
        for side, price, size in (("bid", m.bid_price, m.bid_size), ("ask", m.ask_price,
                                                                     m.ask_size)):
            for i in range(levels):
                have = i < price.shape[1]
                columns[f"{side}_price_{i + 1}"] = price[:, i] if have else np.nan
                columns[f"{side}_size_{i + 1}"] = size[:, i] if have else np.nan
        books.append(pl.DataFrame(columns).fill_nan(None))
        prints.append(pl.DataFrame({
            "market": [m.name] * len(m.trade_time), "time": m.trade_time,
            "price": m.trade_price, "size": m.trade_size,
            "side": np.where(m.trade_sign > 0, "buy", "sell")}))
        if m.value is not None:
            values.append(pl.DataFrame({"market": [m.name] * len(m.value_time),
                                        "time": m.value_time, "value": m.value}))
        if m.scores is not None:
            scores.append(pl.DataFrame({"market": [m.name] * len(m.scores),
                                        "time": m.scores[:, 0], "home": m.scores[:, 1],
                                        "away": m.scores[:, 2]}))
    save(pl.concat(books, how="vertical_relaxed"), "book")
    save(pl.concat(prints, how="vertical_relaxed"), "trades")
    if values:
        save(pl.concat(values, how="vertical_relaxed"), "value")
    if scores:
        save(pl.concat(scores, how="vertical_relaxed"), "scores")
    lines = [f"tick = {tape.options.tick}", f'returns = "{tape.options.returns}"']
    if tape.options.payout is not None:
        lines.append(f"payout = {tape.options.payout}")
    (directory / "tape.toml").write_text("\n".join(lines) + "\n")


# Labels that are times from a market's start, moved with it when a market is cut.
TIME_LABELS = ("kickoff", "kickoff_recorded")


def window(t: Tape, start: float, end: float | None = None) -> Tape:
    """Each market cut to the part from `start` to `end` seconds (to its end if None), with times
    counted from `start`: before kickoff and during play, say, measured apart. The book as it
    stood at `start` is kept as the first snapshot; the outcome and labels are kept, time labels
    (`TIME_LABELS`) counted from `start` too, and only the scores within the part."""
    cut = []
    for m in t.markets:
        stop = m.duration if end is None else min(end, m.duration)
        if stop <= start:
            continue
        first = max(np.searchsorted(m.book_time, start, side="right") - 1, 0)
        books = np.arange(len(m.book_time))
        keep = (books >= first) & (m.book_time <= stop)
        prints = (m.trade_time >= start) & (m.trade_time <= stop)
        book_time = np.maximum(m.book_time[keep] - start, 0.0)
        value_time = value = None
        if m.value is not None:
            first_value = max(np.searchsorted(m.value_time, start, side="right") - 1, 0)
            chosen = (np.arange(len(m.value_time)) >= first_value) & (m.value_time <= stop)
            value_time = np.maximum(m.value_time[chosen] - start, 0.0)
            value = m.value[chosen]
        cut.append(Market(
            name=m.name, duration=stop - start, book_time=book_time,
            bid_price=m.bid_price[keep], bid_size=m.bid_size[keep],
            ask_price=m.ask_price[keep], ask_size=m.ask_size[keep],
            trade_time=m.trade_time[prints] - start, trade_price=m.trade_price[prints],
            trade_size=m.trade_size[prints], trade_sign=m.trade_sign[prints],
            outcome=m.outcome, value_time=value_time, value=value,
            labels={k: (v - start if k in TIME_LABELS and isinstance(v, (int, float))
                        and not isinstance(v, bool) else v) for k, v in m.labels.items()},
            scores=None if m.scores is None else
            m.scores[(m.scores[:, 0] >= start) & (m.scores[:, 0] <= stop)]
            - np.array([start, 0.0, 0.0])))
    return Tape(cut, t.options)


def around(t: Tape, label: str, before: float, after: float | None) -> Tape:
    """Each market cut to the span from `before` seconds ahead of the time a label of its own
    gives, such as a game's kickoff, to `after` seconds past it (to its end if None), each end
    kept within the market. Markets without the label are left out."""
    cut = []
    for m in t.markets:
        at = m.labels.get(label)
        if at is None or isinstance(at, bool) or not np.isfinite(float(at)):
            continue
        start = max(float(at) - before, 0.0)
        end = None if after is None else float(at) + after
        cut += window(Tape([m], t.options), start, end).markets
    return Tape(cut, t.options)


def phase(t: Tape, label: str, after: bool) -> Tape:
    """Each market cut at its own time given by a label, such as a game's kickoff: the part before
    it, or the part from it on. Markets without the label are left out."""
    cut = []
    for m in t.markets:
        at = m.labels.get(label)
        if at is None or not np.isfinite(float(at)):
            continue
        part = window(Tape([m], t.options), float(at), None) if after else window(
            Tape([m], t.options), 0.0, float(at))
        cut += part.markets
    return Tape(cut, t.options)


def options_for(scenario: str) -> Options:
    """How a scenario's prices are read: a prediction market's in cents of a unit that pays 100,
    by differences; any other market's by logarithms."""
    parsed = tomllib.loads(scenario)
    if "prediction" in parsed or "game" in parsed:
        return Options(tick=1.0, returns="difference", payout=100.0)
    return Options(tick=1.0, returns="log")


def simulated_market(tool: Path, job: Run, scratch: Path, name: str,
                     labels: dict | None = None,
                     sample_interval: float = SAMPLE_INTERVAL) -> Market:
    """One run of a scenario as a market of a tape: its book sampled every `sample_interval`
    seconds, every print, and its true value where it has one."""
    depth, prices, log = (scratch / f"{name}.{kind}.csv" for kind in ("depth", "prices", "log"))
    interval = f"{round(sample_interval * 1e6)}us"
    result = run(tool, job, log=log, log_only="trade", prices=prices,
                 extra=["--depth", str(depth), "--sample-interval", interval])
    with depth.open() as header:
        columns = header.readline().strip().split(",")
    snaps = pl.read_csv(depth, schema={c: pl.Float64 for c in columns})
    levels = _levels(snaps, "bid")
    prints = trades(read_log(log))
    sampled = read_prices(prices)
    for path in (depth, prices, log):
        path.unlink()
    book_time = snaps["time"].to_numpy() / 1e9
    has_value = "value" in sampled.columns
    final = result.get("final_value")
    labels = dict(labels or {})
    scores = None
    if "lead" in sampled.columns:
        # A game: its scores from the changes in the lead, and its kickoff as a label, as a
        # recording of games has them.
        lead = sampled["lead"].to_numpy().astype(float)
        changed = np.flatnonzero(np.diff(lead) != 0) + 1
        home = np.cumsum(np.clip(np.diff(lead, prepend=0.0), 0, None))
        away = np.cumsum(np.clip(-np.diff(lead, prepend=0.0), 0, None))
        scores = np.column_stack([sampled["time"].to_numpy()[changed] / 1e9, home[changed],
                                  away[changed]])
        game = tomllib.loads(job.scenario).get("prediction", {}).get("game", {})
        labels.setdefault("kickoff", _seconds(game.get("start", "0s")))
    return Market(
        name=name, duration=result["duration_ns"] / 1e9, book_time=book_time,
        bid_price=_matrix(snaps, [f"bid_price_{i}" for i in range(1, levels + 1)]),
        bid_size=_matrix(snaps, [f"bid_quantity_{i}" for i in range(1, levels + 1)]),
        ask_price=_matrix(snaps, [f"ask_price_{i}" for i in range(1, levels + 1)]),
        ask_size=_matrix(snaps, [f"ask_quantity_{i}" for i in range(1, levels + 1)]),
        trade_time=prints["time"].to_numpy() / 1e9,
        trade_price=prints["price"].to_numpy().astype(float),
        trade_size=prints["quantity"].to_numpy().astype(float),
        trade_sign=np.where(prints["side"].to_numpy() == "buy", 1.0, -1.0),
        outcome=float(final) if final is not None and options_for(job.scenario).payout else None,
        value_time=sampled["time"].to_numpy() / 1e9 if has_value else None,
        value=sampled["value"].to_numpy().astype(float) if has_value else None,
        labels=labels | {"seed": job.seed} | outcomes(result), scores=scores)


def _seconds(text: str) -> float:
    """A scenario's duration as seconds."""
    units = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0, "m": 60.0, "h": 3600.0}
    for unit in ("ns", "us", "ms", "s", "m", "h"):
        if text.endswith(unit) and text[: -len(unit)].replace(".", "", 1).isdigit():
            return float(text[: -len(unit)]) * units[unit]
    raise ValueError(f"not a duration: {text!r}")


def outcomes(result: dict) -> dict:
    """What each group of a run made after fees, valued at the true value where the run has one,
    and how many lots it traded: labels for who profited in a simulated market."""
    found = {}
    value = result.get("final_value")
    for group in result["groups"]:
        name = group["name"].replace(" ", "_")
        if value is not None:
            found[f"pnl_{name}"] = float(pnl_at_value(group, value))
        else:
            found[f"pnl_{name}"] = float(group["pnl"]) - float(group.get("fees", 0.0))
        found[f"traded_{name}"] = int(group["traded"])
    return found


def simulate(tool: Path, scenario: str, seeds, *, labels: dict | None = None,
             workers: int | None = None, sample_interval: float = SAMPLE_INTERVAL) -> Tape:
    """Runs a scenario, given as TOML text, once for each seed, as a tape. Each market is named by
    its seed and labelled with `labels` and its seed. Its scenario needs `depth_levels` in its
    `[exchange]` section, for the book."""
    return simulate_runs(tool, [Run(scenario, seed) for seed in seeds], labels=labels,
                         workers=workers, sample_interval=sample_interval)


def simulate_runs(tool: Path, runs: list[Run], *, labels: dict | None = None,
                  workers: int | None = None, sample_interval: float = SAMPLE_INTERVAL) -> Tape:
    """Runs each of several scenarios under its own seed, as one tape, its markets named by their
    seeds; read as the first scenario says."""
    markets = simulate_markets(tool, runs, labels=labels, workers=workers,
                               sample_interval=sample_interval)
    return Tape(markets, options_for(runs[0].scenario))


def simulate_markets(tool: Path, runs: list[Run], *, labels: dict | None = None,
                     workers: int | None = None, sample_interval: float = SAMPLE_INTERVAL,
                     then: Callable[[int, Market], object] | None = None) -> list:
    """Each run as a market, in the order of the runs, or what `then` makes of the run's index and
    its market. The workers only run markets and read them; `then` runs here, on each market as it
    finishes, so that measuring overlaps running the rest without the two contending. Runs may
    share seeds."""
    with tempfile.TemporaryDirectory() as scratch, ThreadPoolExecutor(
            max_workers=workers or os.cpu_count()) as pool:
        def one(index: int, job: Run) -> Market:
            own = Path(scratch) / str(index)
            own.mkdir()
            return simulated_market(tool, job, own, str(job.seed), labels, sample_interval)

        futures = {pool.submit(one, index, job): index for index, job in enumerate(runs)}
        out: list = [None] * len(runs)
        for future in as_completed(futures):
            index = futures[future]
            market = future.result()
            out[index] = then(index, market) if then is not None else market
        return out
