import os
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

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

    def test_several_scenarios_take_the_seeds_in_turn(self):
        # A stand-in for the engine: the probe makes what the scenario's first line says.
        fake = ("import json, sys\n"
                "args = sys.argv\n"
                "made = float(open(args[2]).readline().split('=')[1])\n"
                "group = {'name': 'probe', 'cash': made, 'initial_cash': 0, 'position': 0,\n"
                "         'initial_position': 0, 'fees': 0, 'traded': 1}\n"
                "json.dump({'final_value': 50, 'last_price': 50, 'groups': [group]},\n"
                "          open(args[args.index('--json') + 1], 'w'))\n")
        with tempfile.TemporaryDirectory() as scratch:
            tool = Path(scratch) / "crowdbook"
            tool.write_text(f"#!{sys.executable}\n" + fake)
            os.chmod(tool, 0o755)
            probe = sweep.Probe.of("maker", {"offset": 1.0})
            rows = sweep.run_probe(tool, ["made = 1\n", "made = 10\n", "made = 100\n"], probe,
                                   range(7, 14), latency="1ms", size=1, max_position=10)
            self.assertEqual(rows[:, 0].tolist(), [1, 10, 100, 1, 10, 100, 1])
            folder = Path(scratch) / "twins"
            folder.mkdir()
            for name in ("b.toml", "a.toml", "notes.md"):
                (folder / name).write_text("")
            self.assertEqual([f.name for f in sweep.scenario_files([folder, tool])],
                             ["a.toml", "b.toml", "crowdbook"])
            (Path(scratch) / "empty").mkdir()
            for missing in ("empty", "nowhere"):
                with self.assertRaises(SystemExit):
                    sweep.scenario_files([Path(scratch) / missing])


if __name__ == "__main__":
    unittest.main()
