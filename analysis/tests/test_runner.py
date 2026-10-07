import unittest

from crowdbook_analysis.runner import agent_groups, pnl_at_value


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
    def test_values_the_change_in_position_at_the_given_price(self):
        team = {"cash": -950, "initial_cash": 0, "position": 10, "initial_position": 0}
        self.assertEqual(pnl_at_value(team, 100.0), 50.0)


if __name__ == "__main__":
    unittest.main()
