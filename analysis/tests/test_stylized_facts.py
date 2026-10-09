import unittest

from crowdbook_analysis.large_orders import with_settings
from crowdbook_analysis.stylized_facts import with_group_setting, without_group, without_keys

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

    def test_without_group_drops_those_agent_tables(self):
        edited = without_group(SCENARIO, ("trend",))
        self.assertNotIn("momentum", edited)
        self.assertIn('name = "noise"', edited)
        self.assertNotIn("[[agents]]", without_group(SCENARIO, ("noise", "trend")))
        with self.assertRaises(ValueError):
            without_group(SCENARIO, ("maker",))
        with self.assertRaises(ValueError):
            without_group(SCENARIO, ("trend", "maker"))

    def test_with_group_setting_changes_only_that_group(self):
        text = (SCENARIO + 'threshold = 1.0           # ticks\n\n'
                '[[agents]]\nname = "other"\nthreshold = 1.0\n')
        edited = with_group_setting(text, "trend", "threshold", "2.5")
        self.assertIn('name = "trend"\nthreshold = 2.5\n', edited)
        self.assertIn('name = "other"\nthreshold = 1.0\n', edited)
        with self.assertRaises(ValueError):
            with_group_setting(text, "noise", "threshold", "2.5")


class WithSettingsTest(unittest.TestCase):
    def test_changes_each_setting_and_drops_its_comment(self):
        text = "cancel_rate = 0.2\nparent_tail = 1.5      # the tail\nmax_parent = 5000\n"
        self.assertEqual(
            with_settings(text, [("cancel_rate", 0.2, 0.02), ("parent_tail", 1.5, 2.5)]),
            "cancel_rate = 0.02\nparent_tail = 2.5\nmax_parent = 5000\n",
        )

    def test_insists_on_the_value_it_replaces(self):
        with self.assertRaises(ValueError):
            with_settings("cancel_rate = 0.25\n", [("cancel_rate", 0.2, 0.02)])


if __name__ == "__main__":
    unittest.main()
