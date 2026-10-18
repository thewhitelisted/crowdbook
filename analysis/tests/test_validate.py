import math
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
        original.markets[0].scores = np.array([[10.0, 1.0, 0.0], [20.0, 1.0, 1.0]])
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
            np.testing.assert_allclose(first.scores, [[10, 1, 0], [20, 1, 1]])
            self.assertIsNone(second.scores)
            self.assertIsNone(second.value)
            np.testing.assert_allclose(second.ask_price, original.markets[1].ask_price)
            np.testing.assert_allclose(second.trade_sign, original.markets[1].trade_sign)

    def test_a_csv_column_whole_for_a_while_then_not_reads_as_numbers(self):
        m = market("a", value=np.append(np.full(99, 52.0), 25.5))
        with tempfile.TemporaryDirectory() as scratch:
            tape.write_tape(tape.Tape([m]), scratch, parquet=False)
            path = Path(scratch) / "value.csv"
            path.write_text(path.read_text().replace("52.0", "52"))
            for _ in range(5):  # far more whole rows before the fraction than are sampled
                lines = path.read_text().splitlines()
                path.write_text("\n".join(lines[:1] + lines[1:-1] * 3 + lines[-1:]) + "\n")
            read = tape.read_tape(scratch).markets[0]
        self.assertEqual(read.value[-1], 25.5)

    def test_labels_value_what_each_group_made_at_the_true_value(self):
        group = {"name": "big makers", "cash": -950, "initial_cash": 0, "position": 10,
                 "initial_position": 0, "fees": 2.5, "pnl": 30, "traded": 12}
        self.assertEqual(tape.outcomes({"final_value": 100.0, "groups": [group]}),
                         {"pnl_big_makers": 47.5, "traded_big_makers": 12})
        self.assertEqual(tape.outcomes({"final_value": None, "groups": [group]})
                         ["pnl_big_makers"], 27.5)

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

    def test_a_cut_market_counts_its_time_labels_from_the_cut(self):
        whole = tape.Tape([market(value=np.arange(100.0)), market("late", duration=50.0)])
        whole.markets[0].labels = {"kickoff": 60.0, "side": "home"}
        whole.markets[1].labels = {"kickoff": 45.0}
        cut = tape.window(whole, 10.0).markets[0]
        self.assertEqual(cut.labels, {"kickoff": 50.0, "side": "home"})
        self.assertEqual(whole.markets[0].labels["kickoff"], 60.0)  # the original is untouched
        # Around each market's own kickoff: from 20 s ahead to 15 s past, kept within it.
        trimmed = tape.around(whole, "kickoff", 20.0, 15.0).markets
        self.assertEqual([m.duration for m in trimmed], [35.0, 25.0])
        self.assertEqual([m.labels["kickoff"] for m in trimmed], [20.0, 20.0])
        self.assertEqual(trimmed[0].value[0], 40.0)
        self.assertEqual(len(tape.around(whole, "missing", 1.0, None).markets), 0)

    def test_a_phase_cuts_each_market_at_its_own_label(self):
        first, second = market("a"), market("b")
        first.labels, second.labels = {"kickoff": 30.0}, {"kickoff": 60.0}
        whole = tape.Tape([first, second, market("no label")])
        before = tape.phase(whole, "kickoff", False)
        after = tape.phase(whole, "kickoff", True)
        self.assertEqual([m.duration for m in before.markets], [30.0, 60.0])
        self.assertEqual([m.duration for m in after.markets], [70.0, 40.0])

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

    def test_prints_microseconds_apart_on_one_side_are_one_order(self):
        # A venue that stamps each fill of one order apart: a buy of 3 in three prints 20 us
        # apart, a sell 20 us later, and a buy 2 ms after that.
        m = market(duration=60.0)
        m.trade_time = np.array([10.0, 10.00002, 10.00004, 10.00006, 10.00206])
        m.trade_price = np.array([101.0, 101.0, 102.0, 99.0, 101.0])
        m.trade_size = np.array([1.0, 1.0, 1.0, 4.0, 2.0])
        m.trade_sign = np.array([1.0, 1.0, 1.0, -1.0, 1.0])
        times, sizes, signs, prices = validate._orders(m, same_order=0.001)
        np.testing.assert_allclose(times, [10.0, 10.00006, 10.00206])
        np.testing.assert_array_equal(sizes, [3.0, 4.0, 2.0])
        np.testing.assert_array_equal(signs, [1.0, -1.0, 1.0])
        self.assertAlmostEqual(prices[0], 304.0 / 3)
        # Without a window, as for the engine's own tapes, only prints at one time are one order:
        # orders of others a moment apart stay apart.
        self.assertEqual(len(validate._orders(m)[0]), 5)
        m.trade_time = np.array([10.0, 10.0, 10.0, 10.00006, 10.00206])
        self.assertEqual(len(validate._orders(m)[0]), 3)
        # The tape says which: its same_order reaches the report.
        found = StatisticsTest().measure(m, options=tape.Options(same_order=0.001))
        self.assertAlmostEqual(found["prints_per_order"], 5 / 3)
        # On a tape stamped to the millisecond, prints a millisecond apart are two orders,
        # however their seconds round.
        m.trade_time = np.round(np.arange(100) * 0.001 + 1234.567, 3)
        m.trade_price, m.trade_size = np.full(100, 101.0), np.ones(100)
        m.trade_sign = np.ones(100)
        self.assertEqual(len(validate._orders(m, same_order=0.001)[0]), 100)

    def test_a_tapes_window_for_one_order_reads_back(self):
        with tempfile.TemporaryDirectory() as scratch:
            for same_order in (0.0, 0.001):
                tape.write_tape(tape.Tape([market("a")], tape.Options(same_order=same_order)),
                                scratch)
                self.assertEqual(tape.read_tape(scratch).options.same_order, same_order)
            with open(Path(scratch) / "tape.toml", "a") as file:
                file.write("same_order = -1\n")
            with self.assertRaises(ValueError):
                tape.read_tape(scratch)

    def test_a_rate_of_events_is_compared_by_its_count(self):
        # One order recorded in an hour; twins that make one a minute expect 60 there. The
        # Poisson deviance is 2 (1 ln(1/60) + 59) = 109.8, a distance of +10.5.
        real = {"_orders": 1.0, "_minutes": 60.0}
        twins = {"_orders": 600.0, "_minutes": 600.0, "_orders_dispersion": 1.0}
        self.assertAlmostEqual(validate.count_distance(real, twins, "orders_per_minute"),
                               math.sqrt(2 * (math.log(1 / 60) + 59)))
        # Bursty twins, four times as variable as Poisson, are farther from nothing less.
        bursty = dict(twins, _orders_dispersion=4.0)
        self.assertAlmostEqual(validate.count_distance(real, bursty, "orders_per_minute"),
                               math.sqrt(2 * (math.log(1 / 60) + 59) / 4))
        # Too few is negative; the same rate is 0; none recorded is 2 e.
        self.assertLess(validate.count_distance({"_orders": 90.0, "_minutes": 60.0}, twins,
                                                "orders_per_minute"), 0)
        self.assertAlmostEqual(validate.count_distance({"_orders": 60.0, "_minutes": 60.0},
                                                       twins, "orders_per_minute"), 0.0)
        self.assertAlmostEqual(validate.count_distance({"_orders": 0.0, "_minutes": 60.0},
                                                       twins, "orders_per_minute"),
                               math.sqrt(120))
        self.assertTrue(np.isnan(validate.count_distance({}, twins, "orders_per_minute")))

    def test_dispersion_of_counts_over_markets(self):
        self.assertEqual(validate._dispersion([5, 5, 5], [1, 1, 1]), 1.0)  # never below 1
        # Counts of 0 and 10 at one rate of 5: Pearson's (25 + 25) / 5 over 1 degree = 10.
        self.assertAlmostEqual(validate._dispersion([0, 10], [1, 1]), 10.0)
        self.assertEqual(validate._dispersion([0, 0], [1, 1]), 1.0)

    def test_the_report_and_the_fit_measure_orders_by_their_count(self):
        # A recording with an order in one market of two, an hour each, against twins with an
        # order a second: the bootstrap's tolerance would call that near, the count does not.
        quiet = market(duration=3600.0)
        quiet.trade_time, quiet.trade_price = quiet.trade_time[:1], quiet.trade_price[:1]
        quiet.trade_size, quiet.trade_sign = quiet.trade_size[:1], quiet.trade_sign[:1]
        empty = market(duration=3600.0)
        for name in ("trade_time", "trade_price", "trade_size", "trade_sign"):
            setattr(empty, name, getattr(empty, name)[:0])
        real = validate.summarize("real", tape.Tape([quiet, empty]), validate.Config(bootstrap=20))
        busy = validate.summarize("twins", tape.Tape([market(duration=3600.0)] * 2),
                                  validate.Config(bootstrap=20))
        self.assertEqual(real.point["_orders"], 1.0)
        self.assertAlmostEqual(real.point["_minutes"], 120.0)
        distance = validate.distance(real, busy, "orders_per_minute", True)
        self.assertAlmostEqual(distance, validate.count_distance(real.point, busy.point,
                                                                 "orders_per_minute"))
        self.assertGreater(distance, 50)

    def test_draws_that_all_agree_have_no_spread(self):
        # The mean of draws all 589 is not exactly 589, so their spread rounds to about 1e-16,
        # which as a tolerance made any miss a distance of 1e16.
        same = np.full(200, 589.0)
        self.assertEqual(validate.spread_of(same, True), 0.0)
        self.assertEqual(validate.spread_of(same, False), 0.0)
        self.assertGreater(validate.spread_of(np.array([1.0, 2.0, 3.0]), False), 0.5)
        self.assertTrue(np.isnan(validate.spread_of(np.array([1.0, 2.0]), False)))

    def test_what_resting_orders_earn(self):
        # Every print a buy at the ask, a tick above the mid, in a market that settles at 0.
        found = self.measure(market(spread=2.0, outcome=0.0))
        self.assertAlmostEqual(found["maker_at_trade"], 1.0)
        self.assertAlmostEqual(found["maker_after_1"], 1.0)
        self.assertAlmostEqual(found["maker_at_settlement"], 51.0)

    def test_how_often_the_touch_moves(self):
        still = market(duration=100.0)
        # A book that never moves counts half a move over the 100 one-second steps seen.
        self.assertAlmostEqual(self.measure(still)["touch_moves"], 60 * 0.5 / 100)
        busy = market(duration=100.0)
        busy.bid_price[:, 0] -= np.arange(len(busy.book_time)) % 2  # the bid moves every second
        # 99 moves in 100 steps: the last step, to the end, has no snapshot after the last.
        self.assertAlmostEqual(self.measure(busy)["touch_moves"], 60 * 99 / 100)

    def test_what_a_standing_quote_would_earn(self):
        # Every second a buyer takes at 51, a tick above the mid of 50; at second 10.5 the buyer
        # sweeps on to 54 in the same order. The market settles at 100.
        m = market(spread=2.0, outcome=100.0)
        at = 11
        m = tape.Market(**{**m.__dict__,
                           "trade_time": np.insert(m.trade_time, at, 10.5),
                           "trade_price": np.insert(m.trade_price, at, 54.0),
                           "trade_size": np.insert(m.trade_size, at, 1.0),
                           "trade_sign": np.insert(m.trade_sign, at, 1.0)})
        found = self.measure(m)
        # A tick out the offer is at 51: every order fills it, a lot each, 100 in 100 seconds.
        self.assertAlmostEqual(found["quote_1_fills"], 100 / (100 / 3600) / 2)
        self.assertAlmostEqual(found["quote_1_after_60"], 1.0)
        self.assertAlmostEqual(found["quote_1_settlement"], 51.0 - 100.0)
        # Three out, at 53, only the sweep reaches it; eight out, at 58, nothing does.
        self.assertAlmostEqual(found["quote_3_fills"], 1 / (100 / 3600) / 2)
        self.assertAlmostEqual(found["quote_3_after_60"], 3.0)
        # None counts as half of one over the time seen, so that the rate stays a number.
        self.assertAlmostEqual(found["quote_8_fills"], 0.5 / (100 / 3600) / 2)
        self.assertTrue(np.isnan(found["quote_8_after_60"]))
        self.assertTrue(np.isnan(self.measure(market())["quote_1_settlement"]))
        quiet = market(duration=120.0)
        quiet = tape.Market(**{**quiet.__dict__, "trade_time": np.array([]),
                               "trade_price": np.array([]), "trade_size": np.array([]),
                               "trade_sign": np.array([])})
        self.assertAlmostEqual(self.measure(quiet)["orders_per_minute"], 0.25)

    def test_how_the_mid_follows_a_value_that_steps_away(self):
        # The value steps from 50 to 60 at second 50; the mid never moves.
        value = np.where(np.arange(100) < 50, 50.0, 60.0)
        found = self.measure(market(value=value))
        self.assertAlmostEqual(found["follow_1"], 0.0)
        self.assertGreater(found["value_distance"], 0.0)

    def test_prices_count_in_ticks_whatever_their_unit(self):
        cents = market(spread=2.0, outcome=100.0, value=np.full(100, 60.0))
        dollars = market(spread=2.0, outcome=100.0, value=np.full(100, 60.0))
        for field in ("bid_price", "ask_price", "trade_price", "value"):
            setattr(dollars, field, getattr(dollars, field) / 100)
        dollars.outcome = 1.0
        in_cents = self.measure(cents, options=tape.Options(tick=1.0, payout=100.0))
        in_dollars = self.measure(dollars, options=tape.Options(tick=0.01, payout=1.0))
        for key in ("spread_mean", "maker_at_trade", "maker_at_settlement", "value_distance",
                    "return_sd_1", "brier"):
            self.assertAlmostEqual(in_cents[key], in_dollars[key], places=6, msg=key)

    def test_depth_near_the_mid_turnover_and_gaps(self):
        found = self.measure(market(spread=2.0))
        self.assertAlmostEqual(found["depth_near_3"], 20.0)  # both levels within 2 ticks
        self.assertAlmostEqual(found["depth_near_10"], 20.0)
        self.assertAlmostEqual(found["touch_turnover"], 60 * 3 / 10)  # 3 lots a second
        self.assertAlmostEqual(found["order_gap_median"], 1.0)

    def test_the_touch_against_the_value(self):
        # The value sits 2.5 ticks above the mid, so 1.5 above the ask: the ask is through it.
        found = self.measure(market(spread=2.0, value=np.full(100, 52.5)))
        self.assertAlmostEqual(found["touch_through_value"], 1.0)
        self.assertAlmostEqual(found["touch_through_value_2"], 0.0)

    def test_how_soon_the_touch_and_the_value_move_after_a_score(self):
        m = market(value=np.where(np.arange(100) < 42, 50.0, 70.0))
        m.ask_price[43:, 0] += 3  # the book moves at second 43
        m.scores = np.array([[40.5, 1.0, 0.0]])
        found = self.measure(m)
        self.assertAlmostEqual(found["score_touch_median"], 2.5)
        self.assertAlmostEqual(found["score_value_median"], 1.5)
        self.assertEqual(found["score_touch_within_5"], 1.0)

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
