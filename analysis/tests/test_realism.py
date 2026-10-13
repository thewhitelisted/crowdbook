import unittest

import numpy as np
import polars as pl

from crowdbook_analysis import realism


def window(times, prices, *, yes=1.0, sizes=None, signs=None, length=900.0) -> realism.Window:
    times = np.asarray(times, dtype=float)
    return realism.Window(
        length=length, time=times, price=np.asarray(prices, dtype=float),
        size=np.ones(len(times)) if sizes is None else np.asarray(sizes, dtype=float),
        sign=np.ones(len(times)) if signs is None else np.asarray(signs, dtype=float), yes=yes)


class OrdersTest(unittest.TestCase):
    def test_prints_of_one_sweep_become_one_order_at_their_average_price(self):
        prints = pl.DataFrame({"time": [5, 5, 5, 9, 9], "price": [50, 51, 52, 52, 52],
                               "quantity": [1, 1, 2, 3, 1],
                               "side": ["buy", "buy", "buy", "buy", "sell"]})
        orders = realism.orders_from_trades(prints)
        self.assertEqual(orders["time"].to_list(), [5, 9, 9])
        self.assertEqual(orders["side"].to_list(), ["buy", "buy", "sell"])
        self.assertEqual(orders["size"].to_list(), [4, 3, 1])
        self.assertAlmostEqual(orders["price"][0], (50 + 51 + 2 * 52) / 4)


class CalibrationTest(unittest.TestCase):
    def test_bins_the_last_price_at_each_moment_against_the_outcome(self):
        # Priced 85 the whole window: every moment falls in the 80-to-90 bin.
        yes = [window([0.0], [85.0], yes=1.0) for _ in range(3)]
        no = [window([0.0], [85.0], yes=0.0)]
        table = realism.calibration(yes + no)
        self.assertEqual(set(table["bins"]), {8})
        mean, resolved, count = table["bins"][8]
        self.assertAlmostEqual(mean, 0.85)
        self.assertAlmostEqual(resolved, 0.75)
        self.assertEqual(count, 4 * (realism.MOMENTS - 1))
        self.assertAlmostEqual(table["brier"], 0.75 * 0.15**2 + 0.25 * 0.85**2)

    def test_uses_the_opening_price_before_the_first_order(self):
        prices = realism.price_at(window([0.5], [80.0]), np.array([0.25, 0.5, 0.75]))
        self.assertEqual(list(prices), [50.0, 80.0, 80.0])


class MovesTest(unittest.TestCase):
    def test_measures_changes_by_the_tenth_they_start_in(self):
        # A price that steps up 2 cents at every fifteenth of the window, from 50.
        times = np.arange(1, realism.PARTS + 1) / realism.PARTS
        prices = 50.0 + 2.0 * np.arange(1, realism.PARTS + 1)
        moves = realism.moves_by_tenth([window(times, prices)])
        self.assertTrue(np.allclose(moves["close"][moves["close_count"] > 0], 2.0))
        self.assertEqual(moves["all_count"].sum(), realism.PARTS)


class SizeTest(unittest.TestCase):
    def test_hill_recovers_the_exponent_of_a_pareto_tail(self):
        rng = np.random.default_rng(3)
        sizes = rng.pareto(1.5, 200_000) + 1.0
        self.assertAlmostEqual(realism.hill(sizes), 1.5, delta=0.05)


class SignTest(unittest.TestCase):
    def test_alternating_signs_correlate_negatively_one_apart(self):
        signs = np.array([1.0, -1.0] * 50)
        memory = realism.sign_memory([window(np.linspace(0, 1, 100), np.full(100, 50.0),
                                             signs=signs)])
        self.assertAlmostEqual(memory[1], -1.0, delta=0.02)
        self.assertAlmostEqual(memory[10], 1.0, delta=0.02)


class BounceTest(unittest.TestCase):
    def test_trades_at_a_bid_and_ask_two_cents_apart_imply_that_spread(self):
        # Buys at 51 and sells at 49, in random order around a mid that stays at 50.
        rng = np.random.default_rng(5)
        prices = 50.0 + rng.choice([-1.0, 1.0], 20_000)
        found = realism.bounce([window(np.linspace(0, 1, len(prices)), prices)])
        self.assertAlmostEqual(found["correlation"], -0.5, delta=0.02)
        self.assertAlmostEqual(found["spread"], 2.0, delta=0.05)


class FeeTest(unittest.TestCase):
    def test_a_probability_curve_charges_the_whole_fee_at_50_and_less_toward_the_ends(self):
        fee = realism.taker_fees('[exchange]\ntaker_fee = 1.75\nfee_curve = "probability"\n')
        np.testing.assert_allclose(fee(np.array([50.0, 10.0, 90.0])), [1.75, 0.63, 0.63])
        flat = realism.taker_fees("[exchange]\ntaker_fee = 0.5\n")
        np.testing.assert_allclose(flat(np.array([50.0, 10.0])), [0.5, 0.5])
        np.testing.assert_allclose(realism.taker_fees("seed = 1\n")(np.array([50.0])), [0.0])

    def test_fees_per_share_pool_every_window(self):
        windows = [window([0.1], [50.0], sizes=[4.0]), window([0.2], [50.0], sizes=[6.0])]
        windows[0].fees, windows[1].fees = 7.0, 3.0
        self.assertAlmostEqual(realism.fee_per_share(windows), 1.0)


if __name__ == "__main__":
    unittest.main()
