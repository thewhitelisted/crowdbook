"""crowdbook-tape: a recording in plain CSV, as order-book data usually comes, turned into a tape.

    crowdbook-tape --quotes quotes.csv --trades trades.csv --market 3600 --tick 0.01 --out recording

- quotes: `time`, `bid_price`, `bid_size`, `ask_price`, `ask_size`, and deeper levels as
  `bid_price_2`, `bid_size_2` and so on if there are any; each row the book as it stood from then.
- trades: `time`, `price`, `size`, and `side`, the side that took liquidity, `buy` or `sell`, or
  empty to infer it from the quotes.
- value, optional: `time`, `value`, a reference price to set the market against.

Times are seconds, or another unit with --time-unit, or dates and times as text. A long recording
is cut into markets --market seconds long, each starting from the book and the value as they stood
then, since a tape's statistics are resampled over markets: a day cut into hours is 24 of them. A
stretch with no quotes, trades or values of its own, such as a night the venue was closed, is left
out. The tape reads
with `tape.read_tape`, and every command that takes --real takes it.
"""

import argparse
import re
from pathlib import Path

import numpy as np
import polars as pl

UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "ns": 1e-9}


def _seconds(frame: pl.DataFrame, unit: str) -> pl.DataFrame:
    """The frame with `time` as seconds, a float, whether it came as numbers in `unit` or as text."""
    time = frame["time"]
    if time.dtype == pl.String:
        parsed = time.str.to_datetime(time_unit="ns", time_zone="UTC")
        seconds = parsed.dt.epoch("ns").cast(pl.Float64) * 1e-9
    elif time.dtype.is_temporal():
        seconds = time.dt.epoch("ns").cast(pl.Float64) * 1e-9
    else:
        seconds = time.cast(pl.Float64) * UNITS[unit]
    if seconds.is_null().any() or seconds.is_nan().any():
        raise ValueError("every row needs a time")
    return frame.with_columns(seconds.alias("time")).sort("time", maintain_order=True)


def _levels(quotes: pl.DataFrame) -> list[str]:
    """The book's columns, renamed to the tape's bid_price_1 ... form, in order."""
    columns = []
    level = 1
    while True:
        names = [f"{side}_{what}_{level}" for side in ("bid", "ask") for what in ("price", "size")]
        if level == 1:
            plain = ["bid_price", "bid_size", "ask_price", "ask_size"]
            if all(n in quotes.columns for n in plain):
                names = plain
        if not all(n in quotes.columns for n in names):
            break
        columns.extend(names)
        level += 1
    if not columns:
        raise ValueError("quotes need bid_price, bid_size, ask_price and ask_size")
    unused = [c for c in quotes.columns
              if re.fullmatch(r"(bid|ask)_(price|size)(_\d+)?", c) and c not in columns]
    if unused:
        raise ValueError(f"levels are bid_price or bid_price_1, then bid_price_2 and on; "
                         f"{', '.join(unused)} would be left out")
    return columns


def convert(quotes: pl.DataFrame, trades: pl.DataFrame, *, market: float | None = None,
            value: pl.DataFrame | None = None, unit: str = "s", name: str = "market"
            ) -> dict[str, pl.DataFrame]:
    """The tape's tables, `markets`, `book`, `trades` and, with a value, `value`, from a recording
    whose times are in `unit`, cut into markets `market` seconds long, or one market for all."""
    if unit not in UNITS:
        raise ValueError(f"the time unit is one of {', '.join(UNITS)}, not {unit}")
    if market is not None and not market > 0:
        raise ValueError("a market's length must be positive")
    quotes = _seconds(quotes, unit)
    trades = _seconds(trades, unit)
    value = None if value is None else _seconds(value, unit)
    if quotes.is_empty():
        raise ValueError("the recording has no quotes")
    columns = _levels(quotes)
    book = quotes.select(["time", *columns])
    book = book.rename({c: f"{c}_1" for c in columns if not c[-1].isdigit()})
    times = [f["time"] for f in (book, trades, value) if f is not None and len(f)]
    start = float(min(t.min() for t in times))
    end = float(max(t.max() for t in times))
    span = end - start
    if not span > 0:
        raise ValueError("the recording spans no time")
    length = span if market is None else float(market)
    count = max(1, int(np.ceil(span / length)))

    def index(time: pl.Expr) -> pl.Expr:
        if market is None:
            return pl.lit(0)
        return ((time - start) / length).floor().cast(pl.Int64).clip(0, count - 1)

    names = [f"{name}-{k:04d}" for k in range(count)]
    starts = [start + k * length for k in range(count)]
    durations = [float(min(length, end - s)) if k == count - 1 else float(length)
                 for k, s in enumerate(starts)]

    def place(frame: pl.DataFrame) -> pl.DataFrame:
        """Each row in its market, its time from that market's start."""
        frame = frame.with_columns(index(pl.col("time")).alias("k"))
        return frame.with_columns(
            pl.col("k").replace_strict(range(count), names, return_dtype=pl.String)
            .alias("market"),
            (pl.col("time") - pl.col("k").replace_strict(range(count), starts,
                                                         return_dtype=pl.Float64)).alias("time"))

    def carry(frame: pl.DataFrame) -> pl.DataFrame:
        """The frame placed in its markets, each market after the first opening with the row that
        stood at its start, the last of an earlier market, at time 0."""
        placed = place(frame)
        carried = []
        for k in range(1, count):
            before = placed.filter(pl.col("k") < k).tail(1)
            # A row within a microsecond of the start opens the market itself: times far from 0,
            # such as seconds since 1970, lose that much in the subtraction.
            first = placed.filter((pl.col("k") == k) & (pl.col("time") < 1e-6))
            if len(before) and first.is_empty():
                carried.append(before.with_columns(pl.lit(names[k]).alias("market"),
                                                   pl.lit(0.0).alias("time"),
                                                   pl.lit(k, dtype=pl.Int64).alias("k")))
        return pl.concat([placed, *carried], how="diagonal_relaxed").sort(["k", "time"],
                                                                          maintain_order=True)

    # A market with nothing of its own, such as an hour the venue was closed, is left out.
    held = set().union(*(place(f)["k"].to_list() for f in (book, trades, value) if f is not None))
    keep = [names[k] for k in sorted(held)]
    book = carry(book)
    book_columns = [c for c in book.columns if c not in ("market", "time", "k")]
    out = {
        "markets": pl.DataFrame({"market": names, "duration": durations}),
        "book": book.select(["market", "time", *book_columns]),
    }
    side = (trades["side"].cast(pl.String).str.to_lowercase() if "side" in trades.columns
            else pl.Series("side", [None] * len(trades), dtype=pl.String))
    bad = ~side.is_null() & ~side.is_in(["buy", "sell", ""])
    if bad.any():
        raise ValueError(f"a trade's side is buy, sell or empty, not {side.filter(bad)[0]!r}")
    trades = trades.with_columns(side.replace("", None).alias("side"))
    out["trades"] = place(trades).sort(["k", "time"], maintain_order=True).select(
        ["market", "time", "price", "size", "side"])
    if value is not None:
        out["value"] = carry(value).select(["market", "time", "value"])
    return {table: frame.filter(pl.col("market").is_in(keep)) for table, frame in out.items()}


def write(tables: dict[str, pl.DataFrame], directory: Path, *, tick: float = 1.0,
          returns: str = "difference", parquet: bool = False) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, frame in tables.items():
        if parquet:
            frame.write_parquet(directory / f"{name}.parquet")
        else:
            frame.write_csv(directory / f"{name}.csv")
    (directory / "tape.toml").write_text(f'tick = {tick}\nreturns = "{returns}"\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quotes", type=Path, required=True, help="a CSV of the book over time")
    parser.add_argument("--trades", type=Path, required=True, help="a CSV of the trades")
    parser.add_argument("--value", type=Path, help="a CSV of a reference price, optional")
    parser.add_argument("--out", type=Path, required=True, help="the tape's directory")
    parser.add_argument("--market", type=float,
                        help="seconds a market, cutting the recording into markets that long")
    parser.add_argument("--time-unit", default="s", choices=list(UNITS),
                        help="what numbers in the time columns count (s)")
    parser.add_argument("--tick", type=float, default=1.0, help="the price increment (1)")
    parser.add_argument("--returns", default="difference", choices=["difference", "log"],
                        help="difference for bounded prices such as probabilities, log otherwise")
    parser.add_argument("--name", default="market", help="what the markets are named after")
    parser.add_argument("--parquet", action="store_true", help="write Parquet, not CSV")
    args = parser.parse_args()
    read = lambda path: pl.read_csv(path, infer_schema_length=None)  # noqa: E731
    try:
        tables = convert(read(args.quotes), read(args.trades), market=args.market,
                         value=read(args.value) if args.value else None, unit=args.time_unit,
                         name=args.name)
    except (OSError, ValueError, pl.exceptions.PolarsError) as error:
        parser.error(str(error))
    write(tables, args.out, tick=args.tick, returns=args.returns, parquet=args.parquet)
    markets = tables["markets"]
    print(f"{len(markets)} markets, {markets['duration'].sum():,.0f} s, "
          f"{len(tables['book']):,} book rows and {len(tables['trades']):,} trades in {args.out}")


if __name__ == "__main__":
    main()
