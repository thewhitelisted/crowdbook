"""Running scenarios through the crowdbook command-line tool."""

import json
import os
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DEFAULT_TOOL = REPO / "build" / "release" / "apps" / "crowdbook"


def find_tool(path: str | None) -> Path:
    """The crowdbook executable: `path`, else $CROWDBOOK, else the Release build."""
    tool = Path(path or os.environ.get("CROWDBOOK") or DEFAULT_TOOL)
    if not tool.is_file():
        raise SystemExit(
            f"crowdbook not found at {tool}; build it with `cmake --workflow --preset release` "
            "or pass --crowdbook"
        )
    return tool


@dataclass(frozen=True)
class Run:
    """One run of a scenario, given as TOML text."""

    scenario: str
    seed: int
    duration: str | None = None


def run(
    tool: Path,
    job: Run,
    *,
    log: Path | None = None,
    log_only: str | None = None,
    prices: Path | None = None,
) -> dict:
    """Runs one scenario and returns its results, as written by `crowdbook run --json`.
    Optionally writes its event log (only the `log_only` kinds, if given) and its prices sampled
    every second."""
    with tempfile.TemporaryDirectory() as scratch:
        scenario = Path(scratch) / "scenario.toml"
        scenario.write_text(job.scenario)
        result = Path(scratch) / "result.json"
        command = [str(tool), "run", str(scenario), "--seed", str(job.seed), "--json", str(result)]
        if job.duration:
            command += ["--duration", job.duration]
        if log:
            command += ["--log", str(log)]
            if log_only:
                command += ["--log-only", log_only]
        if prices:
            command += ["--prices", str(prices)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        return json.loads(result.read_text())


def run_all(tool: Path, jobs: list[Run], workers: int | None = None) -> list[dict]:
    """Runs scenarios in parallel, returning results in the order of `jobs`."""
    with ThreadPoolExecutor(max_workers=workers or os.cpu_count()) as pool:
        return list(pool.map(lambda job: run(tool, job), jobs))


def group(result: dict, name: str) -> dict:
    """The results of the agent group called `name`."""
    for candidate in result["groups"]:
        if candidate["name"] == name:
            return candidate
    raise KeyError(name)


def agent_groups(result: dict) -> dict[int, str]:
    """The name of each agent's group, by the agent ids the event log uses."""
    return {agent: each["name"] for each in result["groups"] for agent in each["agent_ids"]}


def pnl_at_value(group_result: dict, value: float) -> float:
    """A group's PnL after fees, with its position valued at `value` instead of the last trade
    price."""
    return (
        (group_result["cash"] - group_result["initial_cash"])
        + (group_result["position"] - group_result["initial_position"]) * value
        - group_result.get("fees", 0.0)
    )
