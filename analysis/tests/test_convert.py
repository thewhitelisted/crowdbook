import tempfile
import unittest
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import convert
from crowdbook_analysis.tape import Market, read_tape


def quotes(times, unit_scale=1.0):
    n = len(times)
    return pl.DataFrame({
        "time": [t * unit_scale for t in times],
        "bid_price": [100.0 + i for i in range(n)], "bid_size": [5.0] * n,
        "ask_price": [101.0 + i for i in range(n)], "ask_size": [7.0] * n,
    })


class ConvertTest(unittest.TestCase):
    def test_one_market_in_milliseconds_reads_back_as_a_tape(self):
        q = quotes([1_000, 2_000, 4_000])
        t = pl.DataFrame({"time": [1_500, 3_000], "price": [101.0, 100.0], "size": [2.0, 1.0],
                          "side": ["buy", "SELL"]})
        tables = convert.convert(q, t, unit="ms")
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch), tick=0.5)
            tape = read_tape(scratch)
        self.assertEqual(len(tape.markets), 1)
        m = tape.markets[0]
        self.assertEqual(m.duration, 3.0)  # from the first event to the last
        np.testing.assert_allclose(m.book_time, [0.0, 1.0, 3.0])
        np.testing.assert_allclose(m.bid_price[:, 0], [100, 101, 102])
        np.testing.assert_allclose(m.trade_time, [0.5, 2.0])
        np.testing.assert_array_equal(m.trade_sign, [1, -1])
        self.assertEqual(tape.options.tick, 0.5)
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch), same_order=0.002)
            self.assertEqual(read_tape(scratch).options.same_order, 0.002)

    def test_a_long_recording_is_cut_into_markets_each_opening_with_the_book_as_it_stood(self):
        q = quotes([0, 1_000, 3_500, 5_000])
        t = pl.DataFrame({"time": [10.0, 3_700.0, 9_000.0], "price": [100.0, 101.0, 103.0],
                          "size": [1.0, 2.0, 3.0], "side": ["buy", "sell", "buy"]})
        tables = convert.convert(q, t, market=3_600, name="day")
        markets = tables["markets"]
        self.assertEqual(markets["market"].to_list(), ["day-0000", "day-0001", "day-0002"])
        self.assertEqual(markets["duration"].to_list(), [3_600.0, 3_600.0, 1_800.0])
        book = tables["book"]
        second = book.filter(pl.col("market") == "day-0001")
        # The book as it stood at 3,600 s, the quote of 3,500 s, at time 0, then the next.
        self.assertEqual(second["time"].to_list(), [0.0, 1_400.0])
        self.assertEqual(second["bid_price_1"].to_list(), [102.0, 103.0])
        third = book.filter(pl.col("market") == "day-0002")
        self.assertEqual(third["time"].to_list(), [0.0])
        self.assertEqual(third["bid_price_1"].to_list(), [103.0])
        trades = tables["trades"]
        self.assertEqual(trades["market"].to_list(), ["day-0000", "day-0001", "day-0002"])
        self.assertEqual(trades["time"].to_list(), [10.0, 100.0, 1_800.0])
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch), parquet=True)
            self.assertEqual(len(read_tape(scratch).markets), 3)

    def test_times_as_text_and_sides_left_to_infer(self):
        q = pl.DataFrame({"time": ["2026-10-01T00:00:00Z", "2026-10-01T00:00:02Z"],
                          "bid_price": [10.0, 11.0], "bid_size": [1.0, 1.0],
                          "ask_price": [12.0, 13.0], "ask_size": [1.0, 1.0]})
        t = pl.DataFrame({"time": ["2026-10-01T00:00:01Z", "2026-10-01T00:00:03Z"],
                          "price": [12.0, 11.0], "size": [1.0, 1.0], "side": ["", None]})
        tables = convert.convert(q, t)
        self.assertEqual(tables["trades"]["time"].to_list(), [1.0, 3.0])
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch))
            m = read_tape(scratch).markets[0]
        # At the ask is a buy, at the bid a sell.
        np.testing.assert_array_equal(m.trade_sign, [1, -1])

    def test_deeper_levels_and_a_value_are_kept(self):
        q = quotes([0, 1]).with_columns(pl.lit(99.0).alias("bid_price_2"),
                                        pl.lit(3.0).alias("bid_size_2"),
                                        pl.lit(102.0).alias("ask_price_2"),
                                        pl.lit(4.0).alias("ask_size_2"))
        t = pl.DataFrame({"time": [0.5], "price": [101.0], "size": [1.0], "side": ["buy"]})
        v = pl.DataFrame({"time": [0.0, 1.0], "value": [100.5, 101.5]})
        tables = convert.convert(q, t, value=v)
        self.assertIn("ask_size_2", tables["book"].columns)
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch))
            m = read_tape(scratch).markets[0]
        self.assertEqual(m.bid_price.shape[1], 2)
        np.testing.assert_allclose(m.value, [100.5, 101.5])

    def test_markets_laid_end_to_end_in_one_recording_come_back_as_they_were(self):
        rng = np.random.default_rng(3)
        markets = []
        for k in range(3):
            n, trades = 40, 25
            book_time = np.concatenate([[0.0], np.sort(rng.uniform(0, 300, n - 1))])
            bid = 1000 + np.cumsum(rng.integers(-2, 3, size=(n, 2)), axis=0).astype(float)
            bid[:, 1] = bid[:, 0] - 1
            markets.append(Market(
                name=f"m{k}", duration=300.0, book_time=book_time, bid_price=bid,
                bid_size=rng.integers(1, 50, (n, 2)).astype(float), ask_price=bid + 2,
                ask_size=rng.integers(1, 50, (n, 2)).astype(float),
                trade_time=np.sort(rng.uniform(0, 300, trades)),
                trade_price=rng.integers(990, 1010, trades).astype(float),
                trade_size=rng.integers(1, 9, trades).astype(float),
                trade_sign=rng.choice([-1, 1], trades),
                value_time=np.arange(0.0, 300.0, 1.0), value=1000 + rng.normal(size=300)))
        base = 1_790_000_000.0  # seconds since 1970, where subtraction loses precision
        q, t, v = [], [], []
        for k, m in enumerate(markets):
            at = base + 300.0 * k
            cols = {"time": m.book_time + at}
            for side, price, size in (("bid", m.bid_price, m.bid_size),
                                      ("ask", m.ask_price, m.ask_size)):
                for i, suffix in enumerate(["", "_2"]):
                    cols[f"{side}_price{suffix}"] = price[:, i]
                    cols[f"{side}_size{suffix}"] = size[:, i]
            q.append(pl.DataFrame(cols))
            t.append(pl.DataFrame({"time": m.trade_time + at, "price": m.trade_price,
                                   "size": m.trade_size,
                                   "side": np.where(m.trade_sign > 0, "buy", "sell")}))
            v.append(pl.DataFrame({"time": m.value_time + at, "value": m.value}))
        tables = convert.convert(pl.concat(q), pl.concat(t), market=300, value=pl.concat(v))
        with tempfile.TemporaryDirectory() as scratch:
            convert.write(tables, Path(scratch))
            back = read_tape(scratch).markets
        self.assertEqual(len(back), 3)
        # The recording's end is its last event, which ends the last market.
        last = markets[-1]
        ends = [300.0, 300.0, max(last.book_time[-1], last.trade_time[-1], last.value_time[-1])]
        for original, got, end in zip(markets, back, ends, strict=True):
            self.assertAlmostEqual(got.duration, end, delta=1e-6)
            np.testing.assert_allclose(got.book_time, original.book_time, atol=1e-6)
            np.testing.assert_array_equal(got.bid_price, original.bid_price)
            np.testing.assert_array_equal(got.ask_size, original.ask_size)
            np.testing.assert_allclose(got.trade_time, original.trade_time, atol=1e-6)
            np.testing.assert_array_equal(got.trade_sign, original.trade_sign)
            np.testing.assert_array_equal(got.value, original.value)

    def test_the_value_is_carried_in_and_counts_toward_the_recordings_span(self):
        q = quotes([10, 100, 250])
        t = pl.DataFrame({"time": [20.0], "price": [101.0], "size": [1.0], "side": ["buy"]})
        v = pl.DataFrame({"time": [0.0, 150.0, 330.0], "value": [1.0, 2.0, 3.0]})
        tables = convert.convert(q, t, market=100, value=v)
        self.assertEqual(tables["markets"]["duration"].to_list(), [100.0, 100.0, 100.0, 30.0])
        value = tables["value"]
        self.assertEqual(value["market"].to_list(), ["market-0000", "market-0001", "market-0001",
                                                     "market-0002", "market-0003", "market-0003"])
        self.assertEqual(value["time"].to_list(), [0.0, 0.0, 50.0, 0.0, 0.0, 30.0])
        self.assertEqual(value["value"].to_list(), [1.0, 1.0, 2.0, 2.0, 2.0, 3.0])
        self.assertEqual(tables["book"].filter(pl.col("market") == "market-0000")["time"]
                         .to_list(), [10.0])

    def test_a_stretch_with_nothing_of_its_own_is_left_out(self):
        q = quotes([0, 30, 7_250])
        t = pl.DataFrame({"time": [10.0, 7_300.0], "price": [101.0, 102.0], "size": [1.0, 1.0],
                          "side": ["buy", "sell"]})
        tables = convert.convert(q, t, market=3_600)
        self.assertEqual(tables["markets"]["market"].to_list(), ["market-0000", "market-0002"])
        for table in ("book", "trades"):
            self.assertNotIn("market-0001", tables[table]["market"].to_list())
        third = tables["book"].filter(pl.col("market") == "market-0002")
        self.assertEqual(third["bid_price_1"].to_list(), [101.0, 102.0])  # carried, then its own

    def test_a_quote_on_a_boundary_is_the_book_the_next_market_opens_with(self):
        start, length = 1_704_097_352.3936195, 0.1
        q = pl.DataFrame({"time": [start, start + 0.05, start + 0.1, start + 0.15],
                          "bid_price": [1.0, 2.0, 3.0, 4.0], "bid_size": [1.0] * 4,
                          "ask_price": [5.0] * 4, "ask_size": [1.0] * 4})
        t = pl.DataFrame({"time": [start + 0.15], "price": [5.0], "size": [1.0],
                          "side": ["buy"]})
        book = convert.convert(q, t, market=length)["book"]
        second = book.filter(pl.col("market") == "market-0001")
        # Rounding places the quote of start + 0.1 in either market; the second opens with it
        # either way.
        self.assertEqual(second["bid_price_1"].to_list(), [3.0, 4.0])

    def test_mistakes_are_named(self):
        t = pl.DataFrame({"time": [0.0], "price": [1.0], "size": [1.0], "side": ["up"]})
        with self.assertRaisesRegex(ValueError, "buy, sell or empty"):
            convert.convert(quotes([0, 1]), t)
        ok = t.with_columns(pl.lit("buy").alias("side"))
        with self.assertRaisesRegex(ValueError, "bid_price"):
            convert.convert(quotes([0]).drop("ask_size"), ok)
        with self.assertRaisesRegex(ValueError, "time unit"):
            convert.convert(quotes([0]), ok, unit="hours")
        with self.assertRaisesRegex(ValueError, "positive"):
            convert.convert(quotes([0]), ok, market=0)
        with self.assertRaisesRegex(ValueError, "spans no time"):
            convert.convert(quotes([0]), ok)
        with self.assertRaisesRegex(ValueError, "needs a time"):
            convert.convert(quotes([0, 1, 2]).with_columns(time=pl.Series([0.0, None, 2.0])), ok)
        zero_based = quotes([0, 1]).rename({c: f"{c}_0" for c in
                                            ("bid_price", "bid_size", "ask_price", "ask_size")})
        with self.assertRaisesRegex(ValueError, "bid_price_0"):
            convert.convert(zero_based.with_columns(
                *(pl.col(f"{c}_0").alias(f"{c}_1")
                  for c in ("bid_price", "bid_size", "ask_price", "ask_size"))), ok)


if __name__ == "__main__":
    unittest.main()
