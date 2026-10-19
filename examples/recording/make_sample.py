"""Writes this directory's sample recording: twelve of the tutorial's five-minute markets, made by
crowdbook itself and laid end to end into an hour, in the shape market data usually comes in. Quotes are the book's best five levels a
side, in dollars at a cent a tick, a row each time they change, empty where a level has no
orders; trades are prints with the side that took liquidity; and a reference price, the
market's value, once a second. All three are stamped with dates and times to the millisecond.

    crowdbook-generate examples/tutorial/spec.toml --markets 12 --seed 2026 \\
        --fixed maker_intensity=0.8,noise_market_rate=0.3,follower_lag=10 --out /tmp/hour
    python examples/recording/make_sample.py /tmp/hour/shard-00000 examples/recording
"""

import gzip
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis.tape import read_tape

START = datetime(2026, 1, 5, 14, 30, tzinfo=timezone.utc).timestamp()
CENTS = 0.01  # dollars a tick


def stamp(seconds: np.ndarray) -> list[str]:
    return [datetime.fromtimestamp(s, timezone.utc).isoformat(timespec="milliseconds")
            .replace("+00:00", "Z") for s in seconds]


def main() -> None:
    tape, out = read_tape(sys.argv[1]), Path(sys.argv[2])
    quotes, trades, values = [], [], []
    for k, m in enumerate(tape.markets):
        at = START + k * m.duration
        columns = {"time": m.book_time + at}
        for level in range(m.bid_price.shape[1]):
            suffix = "" if level == 0 else f"_{level + 1}"  # bid_price, then bid_price_2 and on
            for side, price, size in (("bid", m.bid_price, m.bid_size),
                                      ("ask", m.ask_price, m.ask_size)):
                columns[f"{side}_price{suffix}"] = np.round(price[:, level] * CENTS, 2)
                columns[f"{side}_size{suffix}"] = size[:, level]
        book = pl.DataFrame(columns).fill_nan(None)
        book = book.filter(pl.col("bid_price").is_not_null() & pl.col("ask_price").is_not_null()
                           & (pl.col("time") < at + m.duration))
        # A row only when the book changes, as a feed of its levels sends them.
        levels = pl.exclude("time")
        changed = pl.any_horizontal(levels.ne_missing(levels.shift(1)))
        quotes.append(book.filter(changed))
        trades.append(pl.DataFrame({
            "time": m.trade_time + at, "price": np.round(m.trade_price * CENTS, 2),
            "size": m.trade_size, "side": np.where(m.trade_sign > 0, "buy", "sell")}))
        # Once a second, up to the market's end, where the next market's value takes over.
        whole = (np.abs(m.value_time - np.round(m.value_time)) < 1e-9) & (m.value_time < m.duration)
        values.append(pl.DataFrame({"time": m.value_time[whole] + at,
                                    "value": np.round(m.value[whole] * CENTS, 4)}))
    for name, frames in (("quotes", quotes), ("trades", trades), ("value", values)):
        frame = pl.concat(frames)
        frame = frame.with_columns(pl.Series("time", stamp(frame["time"].to_numpy())),
                                   pl.selectors.contains("size").cast(pl.Int64))
        path = out / f"{name}.csv.gz"
        # No time in the gzip header, so that the files come out the same each time.
        with gzip.GzipFile(path, "wb", mtime=0) as file:
            frame.write_csv(file)
        print(f"{len(frame):,} rows in {path}")


if __name__ == "__main__":
    main()
