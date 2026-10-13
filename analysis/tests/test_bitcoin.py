import math
import unittest

import numpy as np

from crowdbook_analysis import bitcoin


def prices_from(start: int, path: np.ndarray) -> bitcoin.Prices:
    """Prices each second from `start`, without reading any files."""
    prices = object.__new__(bitcoin.Prices)
    prices.start = start
    prices.price = path
    return prices


class UpProbabilityTest(unittest.TestCase):
    def test_is_the_chance_a_driftless_walk_ends_above_where_it_began(self):
        # An hour of returns of ±0.1%, so the volatility is 0.001 per square root of a second;
        # then a window of 100 seconds in which the price sits one standard deviation of what is
        # left above its start from the 64th second on.
        hour = bitcoin.VOLATILITY_WINDOW
        returns = np.tile([0.001, -0.001], hour // 2)
        before = 100.0 * np.exp(np.concatenate([[0.0], np.cumsum(returns)]))
        start_price = before[-1]
        window = np.full(100, start_price)
        window[64:] = start_price * math.exp(0.001 * math.sqrt(100 - 64))
        prices = prices_from(0, np.concatenate([before, window[1:]]))
        found = bitcoin.up_probability(prices, hour, hour + 100)
        self.assertEqual(len(found), 100)
        self.assertAlmostEqual(found[0], 50.0)
        self.assertAlmostEqual(found[64], 84.13, places=1)

    def test_is_unknown_without_prices(self):
        prices = prices_from(0, np.full(10, np.nan))
        self.assertTrue(np.isnan(bitcoin.up_probability(prices, 5_000, 5_010)).all())


if __name__ == "__main__":
    unittest.main()
