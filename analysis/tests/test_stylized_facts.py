import unittest

from crowdbook_analysis.stylized_facts import without_group, without_keys

SCENARIO = """seed = 1

[fundamental]
volatility = 0.2
jump_rate = 0.001
jump_size = 6.0

[[agents]]
type = "zero_intelligence"
name = "noise"
activity_response = 0.8

[[agents]]
type = "momentum"
name = "trend"
"""


class ScenarioEditsTest(unittest.TestCase):
    def test_without_keys_drops_just_those_settings(self):
        edited = without_keys(SCENARIO, ["jump_rate", "jump_size"])
        self.assertNotIn("jump", edited)
        self.assertIn("volatility = 0.2", edited)
        self.assertIn("activity_response = 0.8", edited)

    def test_without_keys_insists_the_settings_are_there(self):
        with self.assertRaises(ValueError):
            without_keys(SCENARIO, ["volatility_response"])

    def test_without_group_drops_one_agent_table(self):
        edited = without_group(SCENARIO, "trend")
        self.assertNotIn("momentum", edited)
        self.assertIn('name = "noise"', edited)
        with self.assertRaises(ValueError):
            without_group(SCENARIO, "maker")


if __name__ == "__main__":
    unittest.main()
