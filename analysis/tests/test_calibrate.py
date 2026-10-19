import math
import tempfile
import tomllib
import unittest
from dataclasses import replace
from pathlib import Path

import numpy as np

from crowdbook_analysis import calibrate, generate, validate

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
        # A value lead needs twins whose value is the recorded one, and nothing else.
        lead = "\n[parameters.value_lead]\nlow = 1\nhigh = 600\n"
        with self.assertRaisesRegex(ValueError, "value_lead leads"):
            spec_from(SPEC + lead)
        with self.assertRaisesRegex(ValueError, "value_lead leads"):
            spec_from("condition = true\n" + SPEC + lead)
        twins = spec_from("condition = true\n" + SPEC + lead,
                          TEMPLATE + "[prediction]\npath = ${value_path}\n")
        self.assertIn("value_lead", [p.name for p in twins.parameters])
        with self.assertRaisesRegex(ValueError, "low must be below high"):
            spec_from(SPEC.replace("high = 1.0", "high = 0.001"))
        with self.assertRaisesRegex(ValueError, "logarithmic range needs low > 0"):
            spec_from(SPEC.replace('kind = "duration"', 'kind = "duration"\nlog = true'))

    def test_a_search_can_start_from_an_earlier_fit(self):
        spec = spec_from()
        started = calibrate.started_from(spec, {"rate": 0.5, "lag": 9.0, "elsewhere": 1.0})
        starts = {p.name: p.start for p in started.parameters}
        self.assertEqual(starts["rate"], 0.5)
        self.assertEqual(starts["lag"], 0.01)  # kept within its range
        self.assertEqual(starts["traders"], spec.start()["traders"])
        self.assertEqual(spec.start()["rate"], 0.1)  # the spec itself is left as it was

    def test_chi_squared_percentile(self):
        # Known values: 3.84 for one degree of freedom, 55.76 for forty.
        self.assertAlmostEqual(calibrate.chi_squared_95(1), 3.84, delta=0.1)
        self.assertAlmostEqual(calibrate.chi_squared_95(40), 55.76, delta=0.1)


class ConditionTest(unittest.TestCase):
    def test_a_target_market_fills_its_twins_template(self):
        from crowdbook_analysis import tape as tapes
        from test_validate import market
        m = market("game 1", duration=7200.0)
        m.labels = {"kickoff": 1800.0, "competition": "a league", "missing": None}
        m.scores = np.array([[2000.0, 1, 0], [2500.0, 1, 0], [3000.0, 1, 2], [3100.0, 2, 2]])
        texts = calibrate.market_texts(m)
        self.assertEqual(texts["duration"], "7200")
        self.assertEqual(texts["kickoff"], "1800")
        long = market("long", duration=1036800.0)
        long.labels = {"kickoff": 5432.1234}
        self.assertEqual(calibrate.market_texts(long)["duration"], "1036800")
        self.assertEqual(calibrate.market_texts(long)["kickoff"], "5432.1234")
        recorded = market("recorded", duration=29051.723802382003)
        recorded.labels = {"kickoff": 29043.62366123}
        self.assertEqual(calibrate.market_texts(recorded)["duration"], "29051.723802")
        self.assertEqual(calibrate.market_texts(recorded)["kickoff"], "29043.623661")
        self.assertEqual(texts["competition"], '"a league"')
        self.assertNotIn("missing", texts)
        script = tomllib.loads("script = " + texts["script"])["script"]
        self.assertEqual(script, [
            {"at": "2000.000s", "side": "home", "points": 1},
            {"at": "3000.000s", "side": "away", "points": 2},
            {"at": "3100.000s", "side": "home", "points": 1}])
        self.assertEqual(calibrate.market_texts(tapes.Market(
            **{**m.__dict__, "scores": None}))["script"], "[]")

    def test_a_target_market_gives_its_twins_its_value_path_and_outcome(self):
        from crowdbook_analysis import tape as tapes
        from test_validate import market
        m = market("contract", duration=120.0)
        m.value_time = np.array([-1.0, 0.0, 10.0, 20.0, 30.5, 40.0])
        m.value = np.array([0.5, 0.52, 0.52, np.nan, 0.5425, 1.2])
        m.outcome = 1.0
        tape = tapes.Tape([m], tapes.Options(tick=0.01, payout=1.0))
        [m] = calibrate.with_openings(tape)
        self.assertEqual(m.labels["value_path"],
                         [(0.0, 50.0), (0.0, 52.0), (30.5, 54.25), (40.0, 100.0)])
        self.assertEqual(m.labels["outcome"], 100.0)
        texts = calibrate.market_texts(m)
        parsed = tomllib.loads(f"path = {texts['value_path']}\noutcome = {texts['outcome']}")
        self.assertEqual(parsed, {"path": [[0, 50], [0, 52], [30.5, 54.25], [40, 100]],
                                  "outcome": 100})
        # Led by 35 s, the twin's value is the one recorded 35 s later: 54.25 is reached at
        # once, from the start, and 100 at 5 s; the settlement is the recording's.
        led = calibrate.market_texts(m, lead=35.0)
        parsed = tomllib.loads(f"path = {led['value_path']}\noutcome = {led['outcome']}")
        self.assertEqual(parsed, {"path": [[0, 54.25], [5, 100]], "outcome": 100})
        self.assertEqual(calibrate.led([(0.0, 50.0), (10.0, 60.0)], 5.0),
                         [(0.0, 50.0), (5.0, 60.0)])
        self.assertEqual(calibrate.led([(0.0, 50.0), (10.0, 60.0)], 0.0),
                         [(0.0, 50.0), (10.0, 60.0)])
        # Led past its whole path, the value is its last from the start.
        self.assertEqual(calibrate.led([(0.0, 50.0), (10.0, 60.0)], 30.0), [(0.0, 60.0)])
        # Two at one time after the start: the later wins, and a value repeated is left out.
        self.assertEqual(calibrate.led([(0.0, 50.0), (10.0, 60.0), (10.0, 50.0), (20.0, 70.0)],
                                       5.0), [(0.0, 50.0), (15.0, 70.0)])
        # The last seconds of a settled market are worth the settlement, as its reference was
        # once the recording ended.
        self.assertEqual(calibrate.led([(0.0, 50.0), (10.0, 97.0)], 5.0, 120.0, 100.0),
                         [(0.0, 50.0), (5.0, 97.0), (115.0, 100.0)])
        # A spec that fits the lead hands it to every twin, without a placeholder of its own.
        spec = calibrate.Spec(template="path = ${value_path}\n",
                              parameters=[calibrate.Parameter("value_lead", 1.0, 60.0, 10.0)])
        self.assertEqual(spec.scenario({"value_lead": 35.0}, m),
                         f"path = {led['value_path']}\n")
        bare = market("bare", duration=60.0)
        [bare] = calibrate.with_openings(tapes.Tape([bare], tapes.Options(payout=100.0)))
        self.assertEqual(calibrate.market_texts(bare)["value_path"], "[]")
        self.assertNotIn("outcome", bare.labels)
        unsettled = market("unsettled", duration=60.0)
        unsettled.value_time, unsettled.value = np.array([0.0, 30.0]), np.array([40.0, 70.0])
        [unsettled] = calibrate.with_openings(tapes.Tape([unsettled],
                                                         tapes.Options(payout=100.0)))
        self.assertEqual(unsettled.labels["outcome"], 70.0)
        early, late = market("early", duration=60.0), market("late", duration=60.0)
        early.labels, late.labels = {"kickoff": -30.0}, {"kickoff": 90.0}
        calibrate.with_openings(tapes.Tape([early, late], tapes.Options(payout=100.0)))
        self.assertEqual((early.labels["kickoff"], early.labels["kickoff_recorded"]), (0.0, -30.0))
        self.assertEqual(late.labels["kickoff"], 60.0)

    def test_a_phase_fits_the_part_of_each_twin_its_recorded_market_has(self):
        from crowdbook_analysis import tape as tapes
        from crowdbook_analysis import validate
        from test_validate import market
        template = TEMPLATE.replace('duration = "60s"', 'duration = "${duration}s"')
        spec = spec_from('condition = true\nphase = "from kickoff"\n' + SPEC, template)
        self.assertEqual(spec.phase, ("kickoff", True))
        with self.assertRaisesRegex(ValueError, "phase is"):
            spec_from('phase = "after kickoff"\n' + SPEC)
        early, late = market("early", duration=100.0), market("late", duration=100.0)
        early.labels = {"kickoff": 20.0, "value_path": [(0.0, 50.0)]}
        late.labels = {"kickoff": 70.0}
        evaluator = calibrate.Evaluator(None, spec, validate.Config(), [early, late])
        # Twins of the two, in the order the evaluator runs them: a twin is cut at its recorded
        # market's kickoff even where its run gives one of its own, and other labels of the run
        # win.
        twins = [market("1"), market("2"), market("3")]
        twins[2].labels = {"kickoff": 90.0, "seed": 3}
        for index, twin in enumerate(twins):
            evaluator._label_twin(index, twin)
        self.assertEqual([t.labels["kickoff"] for t in twins], [20.0, 70.0, 20.0])
        self.assertEqual(twins[2].labels["seed"], 3)
        self.assertNotIn("value_path", twins[0].labels)
        cut = spec.cut(tapes.Tape(twins))
        self.assertEqual([m.duration for m in cut.markets], [80.0, 30.0, 80.0])
        # A recorded market without the label is left out, and so are its twins.
        late.labels = {}
        orphan = market("4")
        orphan.labels = {"kickoff": 5.0}
        evaluator._label_twin(1, orphan)
        self.assertEqual(spec.cut(tapes.Tape([orphan])).markets, [])

    def test_a_twin_led_ahead_of_its_reference_is_measured_against_that_reference(self):
        from crowdbook_analysis import tape as tapes
        from crowdbook_analysis import validate
        from test_validate import market
        # Recorded in dollars, the reference reaches the twin in cents, as its value path does,
        # and in the twin's own units: its contract pays 100.
        recorded = market("recorded", duration=100.0)
        recorded.value_time, recorded.value = np.array([0.0, 50.0]), np.array([0.4, 0.6])
        [recorded] = calibrate.with_openings(tapes.Tape([recorded], tapes.Options(payout=1.0)))
        twin = market("twin", duration=100.0)
        twin.value_time, twin.value = np.array([0.0, 10.0]), np.array([40.0, 60.0])
        template = TEMPLATE.replace('duration = "60s"', 'duration = "${duration}s"')
        plain = spec_from("condition = true\n" + SPEC, template)
        calibrate.Evaluator(None, plain, validate.Config(), [recorded])._label_twin(0, twin, 100.0)
        self.assertEqual(twin.value_time.tolist(), [0.0, 10.0])  # its own, without a lead
        led = replace(plain, parameters=plain.parameters + [
            calibrate.Parameter(calibrate.VALUE_LEAD, 1.0, 120.0, 40.0)])
        calibrate.Evaluator(None, led, validate.Config(), [recorded])._label_twin(0, twin, 100.0)
        self.assertEqual(twin.value_time.tolist(), [0.0, 50.0])
        self.assertEqual(twin.value.tolist(), [40.0, 60.0])

    def test_around_trims_each_recorded_market_before_the_fit(self):
        from crowdbook_analysis import tape as tapes
        from test_validate import market
        spec = spec_from('around = { label = "kickoff", before = 30, after = 20 }\n' + SPEC)
        self.assertEqual(spec.around, ("kickoff", 30.0, 20.0))
        m = market("game", duration=200.0)
        m.labels = {"kickoff": 100.0}
        [trimmed] = spec.trim(tapes.Tape([m])).markets
        self.assertEqual((trimmed.duration, trimmed.labels["kickoff"]), (50.0, 30.0))
        self.assertEqual(spec_from('around = { label = "kickoff", before = 30 }\n'
                                   + SPEC).around, ("kickoff", 30.0, None))
        with self.assertRaisesRegex(ValueError, "around is"):
            spec_from('around = { label = "kickoff" }\n' + SPEC)

    def test_writes_the_twin_of_every_recorded_market(self):
        from test_validate import market
        spec = spec_from("condition = true\n" + SPEC,
                         TEMPLATE.replace('duration = "60s"', 'duration = "${duration}s"'))
        values = {p.name: p.start for p in spec.parameters}
        with tempfile.TemporaryDirectory() as scratch:
            paths = calibrate.write_twins(spec, values, [market("game/1", duration=30.0),
                                                         market("game 2", duration=45.0)],
                                          Path(scratch) / "twins")
            self.assertEqual([p.name for p in paths], ["0000_game_1.toml", "0001_game_2.toml"])
            self.assertEqual(tomllib.loads(paths[1].read_text())["duration"], "45s")
            self.assertNotIn("ahead of the recorded reference", paths[1].read_text())
            # A twin led ahead of its reference says so, and by how much.
            led = calibrate.write_twins(spec, values | {"value_lead": 90.0},
                                        [market("game", duration=30.0)], Path(scratch) / "led")
            self.assertIn("runs 90 s ahead of the recorded reference", led[0].read_text())
            # A later fit of one market leaves no twin of the earlier fit behind.
            again = calibrate.write_twins(spec, values, [market("other", duration=20.0)],
                                          Path(scratch) / "twins")
            self.assertEqual(sorted(p.name for p in (Path(scratch) / "twins").iterdir()),
                             [p.name for p in again])

    def test_the_reference_twins_fill_and_can_fit_their_makers_count_and_memory(self):
        from test_validate import market
        examples = Path(__file__).resolve().parents[2] / "examples" / "calibration"
        spec = calibrate.read_spec(examples / "reference_twins_spec.toml")
        recorded = market("game", duration=7200.0)
        recorded.labels = {"kickoff": 3600.0, "opening_probability": 0.4,
                           "value_path": [(0.0, 40.0), (600.0, 42.0)], "outcome": 100.0}
        values = spec.start()
        plain = tomllib.loads(spec.scenario(values, recorded))
        makers = next(a for a in plain["agents"] if a["name"] == "makers")
        self.assertEqual((makers["count"], makers["error_memory"]), (3, "120s"))
        fitted = replace(spec, parameters=spec.parameters + [
            calibrate.Parameter("maker_count", 1, 5, 2, log=False, whole=True),
            calibrate.Parameter("maker_error_memory", 5, 3600, 240, kind="duration"),
            calibrate.Parameter(calibrate.VALUE_LEAD, 1, 900, 60)])
        scenario = tomllib.loads(fitted.scenario(fitted.start(), recorded))
        makers = next(a for a in scenario["agents"] if a["name"] == "makers")
        self.assertEqual((makers["count"], makers["error_memory"]), (2, "240s"))
        self.assertEqual(scenario["prediction"]["path"], [[0, 40], [540, 42], [7140, 100]])

    def test_writes_the_settings_as_toml(self):
        values = {"rate": np.float64(0.25), "traders": 30.0, "tiny": np.float64(1e-7)}
        self.assertEqual(tomllib.loads(calibrate.settings_text(values)),
                         {"rate": 0.25, "traders": 30.0, "tiny": 1e-7})

    def test_a_conditioned_spec_may_use_a_markets_own_values(self):
        template = TEMPLATE.replace('duration = "60s"', 'duration = "${duration}s"')
        with self.assertRaisesRegex(ValueError, "condition = true"):
            spec_from(template=template)
        spec = spec_from("condition = true\n" + SPEC, template)
        self.assertTrue(spec.condition)
        from test_validate import market
        scenario = spec.scenario(spec.start(), market("m", duration=90.0))
        self.assertIn('duration = "90s"', scenario)

    def test_a_placeholder_can_say_what_to_write_where_nothing_sets_it(self):
        template = TEMPLATE + 'side = ${side:"home"}\n'
        spec = spec_from(template=template)  # no condition needed: it has a default
        self.assertIn('side = "home"', spec.scenario(spec.start()))
        conditioned = spec_from("condition = true\n" + SPEC, template)
        from test_validate import market
        away = market("m")
        away.labels = {"side": "away"}
        self.assertIn('side = "away"', conditioned.scenario(conditioned.start(), away))


class FitDrawsTest(unittest.TestCase):
    def test_draws_spread_about_the_fit_as_its_intervals_say(self):
        spec = spec_from()
        rows = [{"name": "traders", "fitted": 10.0, "low": 9.0, "high": 11.0},
                {"name": "rate", "fitted": 0.1, "low": 0.09, "high": 0.11},
                {"name": "lag", "fitted": 0.002, "low": None, "high": None}]
        draws = calibrate.draw_settings(spec, rows, 3000, widen=1.0)
        rates = np.log([d["rate"] for d in draws])
        self.assertAlmostEqual(np.median(rates), np.log(0.1), delta=0.01)
        # Logarithmic: the interval's half width in the log is the standard deviation.
        self.assertAlmostEqual(np.std(rates), (np.log(0.11) - np.log(0.09)) / 2, delta=0.01)
        self.assertTrue(all(d["lag"] == 0.002 for d in draws))
        wider = calibrate.draw_settings(spec, rows, 3000, widen=2.0)
        self.assertGreater(np.std(np.log([d["rate"] for d in wider])), 1.8 * np.std(rates))


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

    def points(self, candidates, seeds):
        from crowdbook_analysis import validate
        return [validate.measure(validate.pieces(self.tape(values, seeds), self.config),
                                 self.config) for values in candidates]


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
        self.assertEqual(data["calibration"]["refits"], 0)
        refitted, refit_data, _ = calibrate.calibrate(spec, evaluator, real, truth=truth,
                                                      log=lambda line: None, refits=2,
                                                      refit_generations=1)
        self.assertIn("searches again on fresh seeds", refitted)
        self.assertEqual(refit_data["calibration"]["refits"], 2)

    def test_a_conditioned_fit_writes_the_twin_of_its_first_market(self):
        from crowdbook_analysis import validate
        from test_validate import market
        template = TEMPLATE.replace('duration = "60s"', 'duration = "${duration}s"')
        spec = spec_from("condition = true\n" + SPEC.replace(
            "seeds = 10", "seeds = 2\ngenerations = 2\nheld_out = 2"), template)
        spec.statistics = ["spread_mean"]
        config = validate.Config(bootstrap=10)
        targets = [market(str(i), duration=50.0 + i) for i in range(4)]
        evaluator = FakeEvaluator(None, spec, config, targets)
        real = validate.summarize("real", evaluator.tape(spec.start(), range(900, 903)), config)
        _, _, scenario = calibrate.calibrate(spec, evaluator, real, log=lambda line: None)
        self.assertIn("twin of the recorded market 0", scenario)
        self.assertIn('duration = "50s"', scenario)


class TargetTest(unittest.TestCase):
    def summary(self, draws: dict) -> validate.Summary:
        return validate.Summary("s", 30, {k: float(np.nanmean(v)) for k, v in draws.items()},
                                {k: np.asarray(v, dtype=float) for k, v in draws.items()}, [])

    def test_leaves_out_what_too_few_recorded_markets_measure(self):
        rng = np.random.default_rng(1)
        spread = rng.normal(3.0, 0.1, 200)
        # One market of thirty alone has an order: left out of a third of the resamples.
        size = np.where(rng.random(200) < 0.35, np.nan, 589.0 + rng.normal(0, 1, 200))
        real = self.summary({"spread_mean": spread, "order_size_median": size})
        start = self.summary({"spread_mean": spread + 1,
                              "order_size_median": 5.0 + rng.normal(0, 1, 200)})
        target = calibrate.make_target(real, start, None, validate.Config())
        self.assertEqual(target.keys, ["spread_mean"])
        # Measured in nearly every resample, it is fitted.
        size[np.isnan(size)] = 589.0
        size[:10] = np.nan
        real = self.summary({"spread_mean": spread, "order_size_median": size})
        target = calibrate.make_target(real, start, None, validate.Config())
        self.assertEqual(target.keys, ["spread_mean", "order_size_median"])

    def test_a_rate_of_events_counts_as_a_count_in_the_fit(self):
        rng = np.random.default_rng(2)
        draws = {"orders_per_minute": np.exp(rng.normal(np.log(0.01), 1.0, 200))}
        real = self.summary(draws)
        real.point.update(_orders=1.0, _minutes=100.0)
        target = calibrate.make_target(real, self.summary(draws), None, validate.Config())
        self.assertEqual(target.keys, ["orders_per_minute"])
        busy = {"orders_per_minute": 1.0, "_orders": 1000.0, "_minutes": 1000.0}
        expected = validate.count_distance(real.point, busy, "orders_per_minute") ** 2
        self.assertAlmostEqual(target.distance(busy), expected)
        # Without the counts, the tolerance of the resamples, as before.
        bare = {"orders_per_minute": 1.0}
        gap = (np.log(1.0) - np.log(real.point["orders_per_minute"])) / target.tolerance[0]
        self.assertAlmostEqual(target.distance(bare), gap**2)
        self.assertGreater(target.distance(busy), target.distance(bare))


class TruthTest(unittest.TestCase):
    def test_known_markets_spread_over_a_twin_specs_recorded_ones(self):
        self.assertEqual(len(calibrate.truth_seeds(400, 0)), 400)
        self.assertEqual(len(calibrate.truth_seeds(400, 34)), 12)  # 408 twins
        self.assertEqual(len(calibrate.truth_seeds(10, 34)), 1)
        self.assertEqual(calibrate.truth_seeds(5, 0).start, calibrate.TRUTH_SEED)

    def test_the_reach_check_judges_a_market_with_its_own_spread(self):
        # A rare event, all but unseen at the start (a spread of 1e-6), seen in the markets
        # judged: from the start's tolerance alone it is a miss of about 10, with the judged
        # markets' own spread about 1.
        rng = np.random.default_rng(3)
        real = TargetTest.summary(self, {"touch_through_value_2": 3e-5 + rng.normal(0, 3e-5, 200)})
        start = TargetTest.summary(self, {"touch_through_value_2": 3e-5 + rng.normal(0, 1e-6, 200)})
        target = calibrate.make_target(real, start, None, validate.Config())
        judged = TargetTest.summary(self, {"touch_through_value_2": 2.2e-4 + rng.normal(0, 2e-4, 200)})
        judged.point["touch_through_value_2"] = 2.2e-4
        self.assertGreater(target.distance(judged.point), 25)
        self.assertLess(target.judged(judged), 2)
        # Its tolerance is the start's, the search's seeds, and the markets judged together.
        key = "touch_through_value_2"
        tolerance = math.hypot(target.tolerance[0], validate.spread_of(judged.draws[key], False))
        self.assertAlmostEqual(target.judged(judged),
                               ((2.2e-4 - real.point[key]) / tolerance) ** 2)

    def test_the_reach_check_counts_the_noise_a_fit_chased(self):
        # Fitted on few seeds, the fit's statistic sits off the recording by the search's seeds'
        # noise, which the start's spread measures; the markets judged are many and steady.
        rng = np.random.default_rng(4)
        real = TargetTest.summary(self, {"spread_mean": 3.0 + rng.normal(0, 0.05, 200)})
        start = TargetTest.summary(self, {"spread_mean": 3.0 + rng.normal(0, 0.2, 200)})
        target = calibrate.make_target(real, start, None, validate.Config())
        judged = TargetTest.summary(self, {"spread_mean": 3.3 + rng.normal(0, 0.01, 200)})
        judged.point["spread_mean"] = 3.3
        self.assertLess(target.judged(judged), 4)  # within chance of the noise the fit chased
        self.assertGreater(validate.distance(real, judged, "spread_mean", False) ** 2, 25)

    def test_the_noise_a_fit_chased_is_measured_at_the_fit(self):
        # A start in a busy regime the fit left spreads widely; the fit on the search's seeds
        # spreads little. The search's noise is the fit's, so a fit off by 0.3 fails, judged
        # with the fit; and where the start spreads little and the fit much, it passes.
        rng = np.random.default_rng(5)
        real = TargetTest.summary(self, {"spread_mean": 3.0 + rng.normal(0, 0.05, 200)})
        wide = TargetTest.summary(self, {"spread_mean": 3.0 + rng.normal(0, 0.5, 200)})
        narrow = TargetTest.summary(self, {"spread_mean": 3.0 + rng.normal(0, 0.01, 200)})
        judged = TargetTest.summary(self, {"spread_mean": 3.3 + rng.normal(0, 0.01, 200)})
        judged.point["spread_mean"] = 3.3
        loose = calibrate.make_target(real, wide, None, validate.Config())
        self.assertLess(loose.judged(judged), 4)
        self.assertGreater(loose.judged(judged, searched=narrow), 25)
        tight = calibrate.make_target(real, narrow, None, validate.Config())
        self.assertGreater(tight.judged(judged), 25)
        searched = TargetTest.summary(self, {"spread_mean": 3.3 + rng.normal(0, 0.2, 200)})
        self.assertLess(tight.judged(judged, searched=searched), 4)
        # Where the fit's seeds hardly measure it, the start's wide spread does not come back.
        unmeasured = TargetTest.summary(self, {"spread_mean": np.full(200, np.nan)})
        self.assertGreater(loose.judged(judged, searched=unmeasured), 25)

    def test_the_reach_check_counts_the_search_seeds_count(self):
        # One order an hour recorded; the fit made 60 over 60 minutes of its search's seeds and
        # 600 over 600 judged: the deviance is over 1 + 60/600 + 60/60.
        real = {"_orders": 1.0, "_minutes": 60.0}
        judged = {"_orders": 600.0, "_minutes": 600.0, "_orders_dispersion": 1.0}
        searched = {"_orders": 60.0, "_minutes": 60.0}
        deviance = 2 * (math.log(1 / 60) + 59)
        self.assertAlmostEqual(validate.count_distance(real, judged, "orders_per_minute",
                                                       searched), math.sqrt(deviance / 2.1))
        self.assertAlmostEqual(validate.count_distance(real, judged, "orders_per_minute"),
                               math.sqrt(deviance / 1.1))
