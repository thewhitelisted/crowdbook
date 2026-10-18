import tempfile
import tomllib
import unittest
from pathlib import Path

import numpy as np

from crowdbook_analysis import calibrate, generate

TEMPLATE = """\
# ${not_a_setting} in a comment is left as it is
duration = "60s"
[[agents]]
type = "zero_intelligence"
count = ${traders}
market_rate = ${rate}
latency = { to_exchange = ${lag}, from_exchange = "1ms" }
"""

SPEC = """\
template = "market.toml"
seeds = 10
statistics = ["spread_mean"]

[parameters.traders]
low = 1
high = 50
whole = true

[parameters.rate]
low = 0.01
high = 1.0
start = 0.1

[parameters.lag]
low = 0.0
high = 0.01
kind = "duration"
"""


def spec_from(text: str = SPEC, template: str = TEMPLATE) -> calibrate.Spec:
    with tempfile.TemporaryDirectory() as scratch:
        (Path(scratch) / "market.toml").write_text(template)
        (Path(scratch) / "spec.toml").write_text(text)
        return calibrate.read_spec(Path(scratch) / "spec.toml")


class SpecTest(unittest.TestCase):
    def test_reads_ranges_and_fills_a_scenario(self):
        spec = spec_from()
        self.assertEqual(spec.seeds, 10)
        self.assertEqual(spec.statistics, ["spread_mean"])
        by_name = {p.name: p for p in spec.parameters}
        self.assertTrue(by_name["rate"].log)
        self.assertFalse(by_name["lag"].log)  # a range from 0 moves by steps
        self.assertAlmostEqual(by_name["traders"].start, (1 * 50) ** 0.5)
        scenario = spec.scenario({"traders": 7.0, "rate": 0.25, "lag": 0.0025})
        self.assertIn("${not_a_setting}", scenario)
        parsed = tomllib.loads(scenario.split("\n", 1)[1])
        self.assertEqual(parsed["agents"][0]["count"], 7)
        self.assertEqual(parsed["agents"][0]["market_rate"], 0.25)
        self.assertEqual(parsed["agents"][0]["latency"]["to_exchange"], "2500us")

    def test_reads_a_window(self):
        self.assertIsNone(spec_from().window)
        self.assertEqual(spec_from("window = [600, 900]\n" + SPEC).window, (600.0, 900.0))
        self.assertEqual(spec_from("window = [600]\n" + SPEC).window, (600.0, None))
        self.assertEqual(spec_from("sample_interval = 1.0\n" + SPEC).sample_interval, 1.0)

    def test_the_unit_box_maps_back_and_forth(self):
        spec = spec_from()
        values = {"traders": 12.0, "rate": 0.3, "lag": 0.004}
        back = spec.from_unit(spec.to_unit(values))
        for name, value in values.items():
            self.assertAlmostEqual(back[name], value)
        clipped = spec.from_unit(np.array([2.0, -1.0, 0.5]))
        self.assertEqual(clipped["traders"], 50.0)
        self.assertAlmostEqual(clipped["rate"], 0.01)

    def test_rejects_specs_that_cannot_work(self):
        with self.assertRaisesRegex(ValueError, "never uses unused"):
            spec_from(SPEC + "\n[parameters.unused]\nlow = 1\nhigh = 2\n")
        with self.assertRaisesRegex(ValueError, "low must be below high"):
            spec_from(SPEC.replace("high = 1.0", "high = 0.001"))
        with self.assertRaisesRegex(ValueError, "logarithmic range needs low > 0"):
            spec_from(SPEC.replace('kind = "duration"', 'kind = "duration"\nlog = true'))

    def test_chi_squared_percentile(self):
        # Known values: 3.84 for one degree of freedom, 55.76 for forty.
        self.assertAlmostEqual(calibrate.chi_squared_95(1), 3.84, delta=0.1)
        self.assertAlmostEqual(calibrate.chi_squared_95(40), 55.76, delta=0.1)


class DrawTest(unittest.TestCase):
    def test_draws_within_ranges_on_their_scales_and_holds_fixed_settings(self):
        spec = spec_from()
        rng = np.random.default_rng(1)
        draws = [generate.draw(spec, rng, {"lag": 0.001}) for _ in range(2000)]
        rates = np.array([d["rate"] for d in draws])
        self.assertTrue(np.all((rates >= 0.01) & (rates <= 1.0)))
        # Log-uniform: half the draws below the geometric middle, 0.1.
        self.assertAlmostEqual(np.mean(rates < 0.1), 0.5, delta=0.04)
        self.assertTrue(all(d["traders"] == round(d["traders"]) for d in draws))
        self.assertTrue(all(d["lag"] == 0.001 for d in draws))


if __name__ == "__main__":
    unittest.main()


class FakeEvaluator(calibrate.Evaluator):
    """Markets whose spread is the setting `traders` / 5 ticks and whose orders come at `rate`,
    with a little noise from the seed, so that a fit has something to find."""

    def tape(self, values, seeds):
        from test_validate import market

        from crowdbook_analysis import tape as tapes
        self.runs += len(seeds)
        made = []
        for seed in seeds:
            rng = np.random.default_rng(seed)
            spread = values["traders"] / 5 + rng.normal(0, 0.05)
            made.append(market(str(seed), spread=spread))
        return tapes.Tape(made)


class CalibrateTest(unittest.TestCase):
    def test_a_whole_fit_runs_and_reports(self):
        from crowdbook_analysis import validate
        spec = spec_from(SPEC.replace("seeds = 10", "seeds = 6\ngenerations = 3\nheld_out = 6"))
        spec.statistics = ["spread_mean", "maker_at_trade"]
        config = validate.Config(bootstrap=10)
        evaluator = FakeEvaluator(None, spec, config)
        truth = spec.start() | {"traders": 20.0}
        real = validate.summarize("truth", evaluator.tape(truth, range(900, 912)), config)
        text, data, scenario = calibrate.calibrate(spec, evaluator, real, truth=truth,
                                                   log=lambda line: None)
        self.assertIn("## Settings", text)
        self.assertIn("Distance on the held-out seeds", text)
        self.assertIn("count =", scenario)
        self.assertEqual(data["calibration"]["compared"], ["spread_mean", "maker_at_trade"])
        self.assertIn("reached", data["calibration"])
