import tomllib
import unittest
from dataclasses import replace

import numpy as np

from crowdbook_analysis import fit


def card(**changes) -> dict:
    """A scorecard with every statistic the fit compares."""
    base = {
        "brier": 0.15, "low": 0.13, "high": 0.85, "orders_per_minute": 70.0,
        "moves": {"close": np.array([11.0] + [20.0] * 8 + [44.0])}, "kurtosis": 3.0,
        "activity": np.array([0.08] + [0.1] * 8 + [0.15]),
        "sizes": {"tail": 7.0, "exponent": 1.4}, "signs": {1: 0.3, 10: 0.12},
        "bounce": {"correlation": -0.33, "spread": 3.4}, "lateness": {"delay": 2},
        "value": {"first": 14.0, "last": 40.0, "kurtosis": 2.0},
    }
    return base | changes


class ScenarioTest(unittest.TestCase):
    def test_writes_a_scenario_file_with_the_settings(self):
        settings = replace(fit.Settings(), crowd=7, crowd_lag=12.5, brokers=3, news_share=0.2)
        scenario = tomllib.loads(fit.scenario(settings))
        self.assertEqual(scenario["duration"], "900s")
        self.assertAlmostEqual(scenario["prediction"]["news_share"], 0.2)
        groups = {group["name"]: group for group in scenario["agents"]}
        self.assertEqual(groups["crowd"]["count"], 7)
        self.assertEqual(groups["crowd"]["lag"], "12500ms")
        self.assertEqual(groups["crowd"]["interval"], "2s")
        self.assertEqual(groups["brokers"]["count"], 3)

    def test_leaves_the_brokers_out_when_there_are_none(self):
        scenario = tomllib.loads(fit.scenario(replace(fit.Settings(), brokers=0)))
        self.assertNotIn("brokers", {group["name"] for group in scenario["agents"]})

    def test_every_candidate_is_a_setting(self):
        names = set(fit.Settings.__dataclass_fields__)
        self.assertLessEqual(set(fit.CANDIDATES), names)


class DistanceTest(unittest.TestCase):
    def test_is_zero_for_the_same_scorecard(self):
        self.assertEqual(fit.distance(card(), card(), fit.TRADER_TERMS + fit.VALUE_TERMS), 0.0)

    def test_counts_each_difference_in_tolerances(self):
        # 0.02 off a tolerance of 0.01 is two tolerances, which counts four.
        found = fit.distance(card(brier=0.17), card(), fit.TRADER_TERMS)
        self.assertAlmostEqual(found, 4.0)

    def test_compares_rates_as_ratios(self):
        twice = fit.distance(card(orders_per_minute=140.0), card(), fit.TRADER_TERMS)
        half = fit.distance(card(orders_per_minute=35.0), card(), fit.TRADER_TERMS)
        self.assertAlmostEqual(twice, half)
        self.assertGreater(twice, 0.0)


if __name__ == "__main__":
    unittest.main()
