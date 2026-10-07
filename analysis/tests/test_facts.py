import math
import unittest

import numpy as np
import polars as pl

from crowdbook_analysis import facts


def book(rows: list[tuple[int, int | None, int | None]]) -> pl.DataFrame:
    """Top-of-book rows of (time, bid, ask), shaped like log.quotes()."""
    frame = pl.DataFrame(
        rows, schema={"time": pl.Int64, "bid_price": pl.Int64, "ask_price": pl.Int64}, orient="row"
    )
    return frame.with_columns(
        mid=(pl.col("bid_price") + pl.col("ask_price")) / 2,
        spread=pl.col("ask_price") - pl.col("bid_price"),
    )


class SampleLastTest(unittest.TestCase):
    def test_takes_the_last_value_at_or_before_each_time(self):
        times = np.array([10, 20, 30])
        values = np.array([1.0, 2.0, 3.0])
        sampled = facts.sample_last(times, values, np.array([5, 10, 15, 20, 35]))
        self.assertTrue(math.isnan(sampled[0]))
        self.assertEqual(list(sampled[1:]), [1.0, 1.0, 2.0, 3.0])

    def test_gives_nan_everywhere_without_any_values(self):
        sampled = facts.sample_last(np.array([], dtype=np.int64), np.array([]), np.array([5, 10]))
        self.assertTrue(np.all(np.isnan(sampled)))


class ReturnStatisticsTest(unittest.TestCase):
    def test_kurtosis_is_near_zero_for_normal_returns_and_positive_for_fat_tails(self):
        rng = np.random.default_rng(1)
        self.assertAlmostEqual(facts.excess_kurtosis(rng.normal(size=200_000)), 0.0, delta=0.05)
        self.assertGreater(facts.excess_kurtosis(rng.standard_t(df=4, size=200_000)), 1.0)

    def test_autocorrelation_of_an_alternating_series_is_minus_one_then_one(self):
        alternating = np.array([1.0, -1.0] * 50)
        acf = facts.autocorrelation(alternating, 2)
        self.assertAlmostEqual(acf[0], -0.99, places=2)
        self.assertAlmostEqual(acf[1], 0.98, places=2)

    def test_tail_distribution_starts_at_one_and_falls(self):
        z, share = facts.tail_distribution(np.array([1.0, -2.0, 3.0, -4.0]))
        self.assertEqual(share[0], 1.0)
        self.assertTrue(np.all(np.diff(share) < 0))
        self.assertTrue(np.all(np.diff(z) >= 0))

    def test_normal_tail_matches_known_values(self):
        self.assertAlmostEqual(facts.normal_tail(np.array([1.96]))[0], 0.05, places=3)


class HorizonTest(unittest.TestCase):
    def test_returns_use_non_overlapping_windows(self):
        mids = np.array([100.0, 101.0, 102.0, 104.0, 108.0])
        self.assertTrue(
            np.allclose(facts.horizon_returns(mids, 2), [np.log(102 / 100), np.log(108 / 102)])
        )

    def test_windows_that_start_or_end_without_a_mid_are_left_out(self):
        mids = np.array([np.nan, 100.0, 101.0, 102.0, 103.0, np.nan, 105.0, 106.0, 107.0])
        # Windows of two samples from the first mid: 100 to 102, then two that touch the gap.
        self.assertTrue(np.allclose(facts.horizon_returns(mids, 2), [np.log(102 / 100)]))
        # A tick a second, but for the gap, which no change may straddle.
        steady = np.array([np.nan, 100.0, 101.0, np.nan, 104.0, 105.0])
        self.assertEqual(facts.volatility_per_root_second(steady, 1, 1.0), 0.0)

    def test_volatility_of_a_random_walk_is_the_same_at_every_horizon(self):
        walk = np.cumsum(np.random.default_rng(3).normal(size=400_000))
        short = facts.volatility_per_root_second(walk, 1, 1.0)
        long = facts.volatility_per_root_second(walk, 100, 1.0)
        self.assertAlmostEqual(short, 1.0, delta=0.01)
        self.assertAlmostEqual(long, 1.0, delta=0.05)

    def test_noise_on_top_of_a_walk_inflates_short_horizons(self):
        rng = np.random.default_rng(4)
        noisy = np.cumsum(rng.normal(size=400_000)) + rng.normal(scale=3.0, size=400_000)
        self.assertGreater(
            facts.volatility_per_root_second(noisy, 1, 1.0),
            2 * facts.volatility_per_root_second(noisy, 100, 1.0),
        )


class SpreadSharesTest(unittest.TestCase):
    def test_weights_each_spread_by_how_long_it_lasted(self):
        quotes = book([(0, 99, 101), (30, 100, 101), (40, None, 101)])
        # Spread 2 for 30 ns, spread 1 for 10 ns, then one-sided until the end.
        self.assertEqual(facts.spread_shares(quotes, 100), {1: 0.25, 2: 0.75})


class ImpactResponseTest(unittest.TestCase):
    def test_measures_the_mid_move_in_each_orders_direction_by_group_and_horizon(self):
        quotes = book([(0, 99, 101), (10, 101, 103), (50, 97, 99)])  # mids 100, 102, 98
        orders = pl.DataFrame({"time": [10, 50], "side": ["buy", "sell"], "group": ["a", "b"]})
        impact = facts.impact_response(orders, quotes, [5, 45], "group", end_ns=60)
        # a's buy at 10: from 100 to 102 after 5 ns (+2), to 98 after 45 (-2). b's sell at 50:
        # from 102 to 98 after 5 ns (+4); the run ends before b sees 45 ns.
        self.assertEqual(
            impact.select("group", "horizon_ns", "move", "orders").rows(),
            [("a", 5, 2.0, 1), ("a", 45, -2.0, 1), ("b", 5, 4.0, 1)],
        )


class MarkoutsTest(unittest.TestCase):
    def test_measures_the_mid_move_in_the_makers_favour_from_the_fill_price(self):
        quotes = book([(0, 99, 101), (10, 101, 103)])  # mids 100, then 102
        fills = pl.DataFrame({"time": [5], "price": [101], "maker_side": ["sell"]})
        result = facts.markouts(fills, quotes, [2, 10, 50], end_ns=20)
        # The maker sold at 101: the mid is 100 at 7 ns (+1 for the maker), 102 at 15 ns (-1),
        # and the run ends before 55 ns.
        self.assertEqual(result.select("horizon_ns", "markout").rows(), [(2, 1.0), (10, -1.0)])


class MeanAndErrorTest(unittest.TestCase):
    def test_averages_over_runs_leaving_out_nan(self):
        mean, error = facts.mean_and_error([1.0, 3.0, float("nan")])
        self.assertEqual(mean, 2.0)
        self.assertAlmostEqual(error, 1.0)

    def test_averages_curves_point_by_point(self):
        mean, error = facts.mean_and_error([[1.0, 10.0], [3.0, 10.0]])
        self.assertEqual(list(mean), [2.0, 10.0])
        self.assertEqual(list(error), [1.0, 0.0])


if __name__ == "__main__":
    unittest.main()
