import tomllib
import unittest

import numpy as np

from crowdbook_analysis import sweep


class SweepTest(unittest.TestCase):
    def test_a_probe_joins_a_scenario_as_its_own_group(self):
        probe = sweep.Probe.of("trend", {"fast_half_life": "2s", "threshold": 0.5})
        scenario = tomllib.loads('[[agents]]\ntype = "momentum"\n'
                                 + probe.section("1ms", 2, 10))
        added = scenario["agents"][-1]
        self.assertEqual(added["type"], "probe")
        self.assertEqual(added["style"], "trend")
        self.assertEqual(added["fast_half_life"], "2s")
        self.assertEqual(added["threshold"], 0.5)
        self.assertEqual(added["account"]["max_order_quantity"], 2)
        self.assertEqual(probe.name, "trend (fast_half_life 2s, threshold 0.5)")

    def test_values_the_probe_at_the_true_value_after_fees(self):
        result = {"final_value": 100, "last_price": 90, "groups": [
            {"name": "probe", "cash": -950, "initial_cash": 0, "position": 10,
             "initial_position": 0, "fees": 2.5, "traded": 10}]}
        self.assertEqual(sweep.made(result), (47.5, 10))
        result["final_value"] = None
        self.assertEqual(sweep.made(result)[0], -52.5)

    def test_flags_only_an_interval_wholly_above_zero(self):
        probe = sweep.Probe.of("maker", {"offset": 1.0})
        paid = sweep.summary(np.array([[10.0, 2], [12.0, 2], [11.0, 2], [9.0, 2]]))
        unclear = sweep.summary(np.array([[10.0, 2], [-12.0, 2], [11.0, 2], [-9.0, 2]]))
        for result, flagged in ((paid, True), (unclear, False)):
            text, found = sweep.report("m", {"searched": {probe: result},
                                             "judged": {probe: result}}, 4, 4)
            self.assertEqual(found, flagged)
        self.assertAlmostEqual(paid["per_lot"], 5.25)

    def test_every_family_is_swept(self):
        families = {p.family for p in sweep.default_probes()}
        self.assertEqual(families, {"maker", "trend", "reversal", "flow"})


if __name__ == "__main__":
    unittest.main()
