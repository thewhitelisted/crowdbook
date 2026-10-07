import tempfile
import unittest
from pathlib import Path

from crowdbook_analysis.runner import Run, agent_groups, pnl_at_value, run


class AgentGroupsTest(unittest.TestCase):
    def test_maps_each_agent_id_to_its_group(self):
        result = {
            "groups": [
                {"name": "maker", "agent_ids": [1]},
                {"name": "noise", "agent_ids": [2, 3]},
            ]
        }
        self.assertEqual(agent_groups(result), {1: "maker", 2: "noise", 3: "noise"})


class PnlAtValueTest(unittest.TestCase):
    def test_values_the_change_in_position_at_the_given_price_after_fees(self):
        team = {"cash": -950, "initial_cash": 0, "position": 10, "initial_position": 0}
        self.assertEqual(pnl_at_value(team, 100.0), 50.0)
        self.assertEqual(pnl_at_value(team | {"fees": 2.5}, 100.0), 47.5)


class RunTest(unittest.TestCase):
    def test_a_failed_run_says_why(self):
        with tempfile.TemporaryDirectory() as scratch:
            tool = Path(scratch) / "crowdbook"
            tool.write_text(
                "#!/bin/sh\necho 'crowdbook: scenario.toml:3: unknown key' >&2\nexit 1\n"
            )
            tool.chmod(0o755)
            with self.assertRaisesRegex(RuntimeError, "scenario.toml:3: unknown key"):
                run(tool, Run(scenario="", seed=1))


if __name__ == "__main__":
    unittest.main()
