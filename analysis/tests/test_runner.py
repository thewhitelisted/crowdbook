import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from crowdbook_analysis import runner
from crowdbook_analysis.runner import Run, agent_groups, find_tool, pnl_at_value, run


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


class FindToolTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        self.installed = self.scratch / "bin" / "crowdbook"
        self.installed.parent.mkdir()
        self.installed.write_text("#!/bin/sh\n")
        self.installed.chmod(0o755)
        # No checkout build, no command beside the package, and only the installed one on the path.
        self.scripts = self.scratch / "scripts"
        self.scripts.mkdir()
        for patch in (mock.patch.object(runner, "DEFAULT_TOOL", self.scratch / "no-build"),
                      mock.patch.object(runner.sysconfig, "get_path",
                                        lambda name: str(self.scripts)),
                      mock.patch.dict(os.environ, {"PATH": str(self.installed.parent)})):
            patch.start()
            self.addCleanup(patch.stop)
        os.environ.pop("CROWDBOOK", None)

    def test_without_a_build_the_installed_command_is_used(self):
        self.assertEqual(find_tool(None), self.installed)

    def test_the_command_beside_the_package_is_found_off_the_path(self):
        # As when a venv's command runs without the venv activated.
        beside = self.scripts / "crowdbook"
        beside.write_text("")
        with mock.patch.dict(os.environ, {"PATH": str(self.scratch)}):
            self.assertEqual(find_tool(None), beside)

    def test_the_checkout_build_comes_before_the_installed_command(self):
        build = self.scratch / "build-crowdbook"
        build.write_text("")
        with mock.patch.object(runner, "DEFAULT_TOOL", build):
            self.assertEqual(find_tool(None), build)

    def test_a_path_given_comes_first_and_is_not_replaced(self):
        os.environ["CROWDBOOK"] = str(self.scratch / "missing")
        with self.assertRaisesRegex(SystemExit, "pip install crowdbook-engine"):
            find_tool(None)
        self.assertEqual(find_tool(str(self.installed)), self.installed)

    def test_nothing_found_says_how_to_get_it(self):
        with mock.patch.dict(os.environ, {"PATH": str(self.scratch)}):
            with self.assertRaisesRegex(SystemExit, "pip install crowdbook-engine"):
                find_tool(None)


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
