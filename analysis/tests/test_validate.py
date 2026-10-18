import tempfile
import unittest
from pathlib import Path

import numpy as np

from crowdbook_analysis import tape, validate


def market(name="m", *, spread=2.0, signs=None, prices=None, outcome=None, value=None,
           duration=100.0) -> tape.Market:
    """A market whose book is quoted around 50 every second with `spread` ticks, and a print
    every second from time 0.5."""
    book_time = np.arange(0.0, duration, 1.0)
    bid = np.full((len(book_time), 2), 50 - spread / 2)
    bid[:, 1] -= 1
    ask = np.full((len(book_time), 2), 50 + spread / 2)
    ask[:, 1] += 1
    sizes = np.full((len(book_time), 2), 10.0)
    trade_time = np.arange(0.5, duration, 1.0)
    signs = np.ones(len(trade_time)) if signs is None else signs
    prices = 50 + signs * spread / 2 if prices is None else prices
    value_time = None if value is None else book_time
    return tape.Market(name=name, duration=duration, book_time=book_time, bid_price=bid,
                       bid_size=sizes, ask_price=ask, ask_size=sizes.copy(),
                       trade_time=trade_time, trade_price=prices,
                       trade_size=np.full(len(trade_time), 3.0), trade_sign=signs,
                       outcome=outcome, value_time=value_time, value=value)


class TapeTest(unittest.TestCase):
    def test_a_tape_reads_back_as_it_was_written(self):
        original = tape.Tape([market("a", outcome=100.0, value=np.full(100, 60.0)),
                              market("b", spread=4.0)],
                             tape.Options(tick=1.0, returns="difference", payout=100.0))
        original.markets[0].labels = {"noise_rate": 0.5}
        for parquet in (False, True):
            with tempfile.TemporaryDirectory() as scratch:
                tape.write_tape(original, scratch, parquet=parquet)
                read = tape.read_tape(scratch)
            self.assertEqual([m.name for m in read.markets], ["a", "b"])
            self.assertEqual(read.options.payout, 100.0)
            first, second = read.markets
            self.assertEqual(first.outcome, 100.0)
            self.assertEqual(first.labels["noise_rate"], 0.5)
            np.testing.assert_allclose(first.value, 60.0)
            self.assertIsNone(second.value)
            np.testing.assert_allclose(second.ask_price, original.markets[1].ask_price)
            np.testing.assert_allclose(second.trade_sign, original.markets[1].trade_sign)

    def test_infers_who_took_liquidity_from_the_quote_before_each_print(self):
        signs = tape.infer_signs(
            time=np.array([1.0, 2.0, 3.0, 4.0]), price=np.array([51.0, 49.0, 50.0, 52.0]),
            book_time=np.array([0.0]), bid=np.array([49.0]), ask=np.array([51.0]))
        np.testing.assert_array_equal(signs, [1, -1, -1, 1])

    def test_a_window_keeps_the_part_asked_for_with_the_book_as_it_stood(self):
        whole = tape.Tape([market(value=np.arange(100.0), outcome=100.0)])
        cut = tape.window(whole, 10.5, 20.0).markets[0]
        self.assertEqual(cut.duration, 9.5)
        self.assertEqual(cut.book_time[0], 0.0)  # the snapshot of second 10, as it stood
        self.assertEqual(len(cut.book_time), 11)
        np.testing.assert_allclose(cut.trade_time, np.arange(0.0, 9.5, 1.0))
        self.assertEqual(cut.value[0], 10.0)
        self.assertEqual(cut.outcome, 100.0)
        self.assertEqual(tape.window(whole, 50.0).markets[0].duration, 50.0)
        self.assertEqual(tape.window(whole, 200.0).markets, [])

    def test_reads_a_tape_with_no_sides_and_no_optional_tables(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch)
            (path / "markets.csv").write_text("market,duration\nx,10\n")
            (path / "book.csv").write_text(
                "market,time,bid_price_1,bid_size_1,ask_price_1,ask_size_1\nx,0,99,5,101,7\n")
            (path / "trades.csv").write_text("market,time,price,size\nx,1,101,2\nx,2,99,1\n")
            read = tape.read_tape(path)
        only = read.markets[0]
        np.testing.assert_array_equal(only.trade_sign, [1, -1])
        self.assertIsNone(only.outcome)
        self.assertEqual(read.options.returns, "difference")


class StatisticsTest(unittest.TestCase):
    def measure(self, *markets, options=tape.Options(), config=validate.Config()):
        ps = validate.pieces(tape.Tape(list(markets), options), config)
        return validate.measure(ps, config)

    def test_the_book_at_the_touch(self):
        found = self.measure(market(spread=2.0))
        self.assertAlmostEqual(found["spread_mean"], 2.0)
        self.assertEqual(found["spread_one_tick"], 0.0)
        self.assertAlmostEqual(found["touch_size_median"], 10.0)
        self.assertAlmostEqual(found["depth_mean"], 20.0)
        self.assertEqual(found["one_sided"], 0.0)

    def test_orders_and_their_signs(self):
        alternating = np.where(np.arange(100) % 2 == 0, 1.0, -1.0)
        found = self.measure(market(signs=alternating))
        self.assertAlmostEqual(found["orders_per_minute"], 60.0)
        self.assertAlmostEqual(found["sign_memory_1"], -1.0, places=1)
        self.assertAlmostEqual(found["sign_memory_10"], 1.0, places=1)
        self.assertAlmostEqual(found["order_size_median"], 3.0)
        self.assertAlmostEqual(found["order_gap_cv"], 0.0)

    def test_what_resting_orders_earn(self):
        # Every print a buy at the ask, a tick above the mid, in a market that settles at 0.
        found = self.measure(market(spread=2.0, outcome=0.0))
        self.assertAlmostEqual(found["maker_at_trade"], 1.0)
        self.assertAlmostEqual(found["maker_after_1"], 1.0)
        self.assertAlmostEqual(found["maker_at_settlement"], 51.0)

    def test_how_the_mid_follows_a_value_that_steps_away(self):
        # The value steps from 50 to 60 at second 50; the mid never moves.
        value = np.where(np.arange(100) < 50, 50.0, 60.0)
        found = self.measure(market(value=value))
        self.assertAlmostEqual(found["follow_1"], 0.0)
        self.assertGreater(found["value_distance"], 0.0)

    def test_pooled_autocorrelation_matches_the_direct_one_for_a_single_series(self):
        rng = np.random.default_rng(3)
        x = np.cumsum(rng.standard_normal(500)) * 0.1 + rng.standard_normal(500)
        pooled = validate._pooled_autocorrelation(
            validate._autocorrelation_sums(x, (1, 5))[None, :], (1, 5))
        centred = x - x.mean()
        for lag, value in zip((1, 5), pooled, strict=True):
            direct = (centred[:-lag] @ centred[lag:] / (len(x) - lag)) / centred.var()
            self.assertAlmostEqual(value, direct, places=2)

    def test_hill_recovers_a_pareto_exponent(self):
        rng = np.random.default_rng(5)
        sizes = (1 - rng.random(200_000)) ** (-1 / 1.5)
        self.assertAlmostEqual(validate.hill(sizes, 0.1), 1.5, delta=0.05)

    def test_a_market_set_beside_itself_is_at_no_distance(self):
        markets = [market(str(i), spread=2.0 + (i % 3)) for i in range(12)]
        config = validate.Config(bootstrap=20)
        summary = validate.summarize("a", tape.Tape(markets), config)
        self.assertEqual(validate.distance(summary, summary, "spread_mean", True), 0.0)
        text, data = validate.report(summary, [summary], config)
        self.assertIn("spread, time-weighted mean", text)
        self.assertEqual(data["summary"][0]["distance"], 0.0)


if __name__ == "__main__":
    unittest.main()
