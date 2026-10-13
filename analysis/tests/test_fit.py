import math
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
        "closing": 0.99, "activity": np.array([0.08] + [0.1] * 8 + [0.15]),
        "sizes": {"tail": 7.0, "exponent": 1.4}, "signs": {1: 0.3, 10: 0.12},
        "bounce": {"correlation": -0.33, "spread": 3.4},
        "lateness": {"delay": 2, "distance": 7.0},
        "value": {"first": 14.0, "last": 40.0, "kurtosis": 2.0},
    }
    return base | changes


class ScenarioTest(unittest.TestCase):
    def test_writes_a_scenario_file_with_the_settings_and_polymarkets_fee(self):
        settings = replace(fit.Settings(), crowd=7, crowd_lag=12.5, brokers=3, news_share=0.2,
                           broker_interval_spread=0.4)
        scenario = tomllib.loads(fit.scenario(settings))
        self.assertEqual(scenario["duration"], "900s")
        self.assertAlmostEqual(scenario["prediction"]["news_share"], 0.2)
        self.assertEqual(scenario["exchange"]["taker_fee"], 1.75)
        self.assertEqual(scenario["exchange"]["fee_curve"], "probability")
        groups = {group["name"]: group for group in scenario["agents"]}
        self.assertEqual(groups["crowd"]["count"], 7)
        self.assertEqual(groups["crowd"]["lag"], "12500ms")
        self.assertEqual(groups["crowd"]["interval"], "10s")
        self.assertEqual(groups["brokers"]["count"], 3)
        self.assertAlmostEqual(groups["brokers"]["interval_spread"], 0.4)

    def test_leaves_the_brokers_out_when_there_are_none(self):
        scenario = tomllib.loads(fit.scenario(replace(fit.Settings(), brokers=0)))
        self.assertNotIn("brokers", {group["name"] for group in scenario["agents"]})


class BoxTest(unittest.TestCase):
    def test_every_fitted_setting_has_a_range_holding_its_start(self):
        start = fit.Settings()
        for name in fit.FITTED:
            span = fit.RANGES[name]
            self.assertLessEqual(span.low, getattr(start, name), name)
            self.assertGreaterEqual(span.high, getattr(start, name), name)

    def test_settings_survive_the_trip_through_the_box(self):
        start = fit.Settings()
        point = fit.to_unit(start)
        self.assertTrue(np.all((point >= 0) & (point <= 1)))
        back = fit.from_unit(point, start)
        for name in fit.FITTED:
            self.assertAlmostEqual(getattr(back, name), getattr(start, name), places=9, msg=name)
            self.assertIs(type(getattr(back, name)), type(getattr(start, name)), name)

    def test_points_outside_the_box_are_clipped_and_whole_numbers_rounded(self):
        point = fit.to_unit(fit.Settings())
        point[fit.FITTED.index("crowd")] = 7.0
        point[fit.FITTED.index("news_rate")] = -1.0
        settings = fit.from_unit(point)
        self.assertEqual(settings.crowd, 40)
        self.assertAlmostEqual(settings.news_rate, fit.RANGES["news_rate"].low)
        self.assertIsInstance(fit.from_unit(fit.to_unit(fit.Settings()) + 0.01).fast, int)


class DistanceTest(unittest.TestCase):
    def test_is_zero_for_the_same_scorecard(self):
        found = fit.statistics(card())
        self.assertEqual(fit.distance(found, found, np.ones_like(found)), 0.0)

    def test_counts_each_difference_in_tolerances(self):
        # 0.02 off a tolerance of 0.01 is two tolerances, which counts four.
        tolerance = np.ones(len(fit.STATISTICS))
        tolerance[3] = 0.01
        found = fit.distance(fit.statistics(card(brier=0.17)), fit.statistics(card()), tolerance)
        self.assertAlmostEqual(found, 4.0)

    def test_compares_rates_as_ratios(self):
        tolerance = np.ones(len(fit.STATISTICS))
        base = fit.statistics(card())
        twice = fit.distance(fit.statistics(card(orders_per_minute=140.0)), base, tolerance)
        half = fit.distance(fit.statistics(card(orders_per_minute=35.0)), base, tolerance)
        self.assertAlmostEqual(twice, half)
        self.assertAlmostEqual(twice, math.log(2) ** 2)

    def test_a_statistic_that_could_not_be_measured_is_far_off(self):
        tolerance = np.ones(len(fit.STATISTICS))
        broken = card(bounce={"correlation": -0.3, "spread": float("nan")})
        self.assertGreater(
            fit.distance(fit.statistics(broken), fit.statistics(card()), tolerance), 1e5)


class SearchTest(unittest.TestCase):
    def test_cma_finds_the_minimum_of_a_stretched_bowl(self):
        center = np.array([0.3, 0.7, 0.5, 0.2])
        scales = np.array([1.0, 10.0, 100.0, 3.0])
        cma = fit.Cma(np.full(4, 0.5), sigma=0.2, seed=3)
        for _ in range(150):
            points = cma.ask()
            cma.tell(points, np.sum(scales * (points - center) ** 2, axis=1))
        np.testing.assert_allclose(cma.mean, center, atol=1e-3)

    def test_uncertainty_is_the_least_squares_covariance_for_a_linear_model(self):
        # Statistics that move linearly with two settings, with independent errors of known
        # size: the sandwich is (J' W J)^-1 when the tolerances are those errors.
        jacobian = np.array([[1.0, 0.0], [0.0, 2.0], [1.0, 1.0]])
        errors = np.array([0.1, 0.2, 0.3])
        found = fit.uncertainty(jacobian, np.diag(errors**2), errors)
        expected = np.linalg.inv(jacobian.T @ np.diag(1 / errors**2) @ jacobian)
        np.testing.assert_allclose(found, expected)


if __name__ == "__main__":
    unittest.main()
