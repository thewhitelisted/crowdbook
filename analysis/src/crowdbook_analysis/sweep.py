"""The no-false-edge check: generic strategies, run small in a market, that should make nothing
after fees where the real market offers nothing.

    uv run --project analysis crowdbook-sweep my_fitted_market.toml
    uv run --project analysis crowdbook-sweep my_fitted_market.toml --seeds 400 --confirm 1600
    uv run --project analysis crowdbook-sweep fit/twins     # every recorded market's twin, in turn

A market fitted to a real one can offer edges the real one does not: a book wider and thinner than
the real one pays a patient maker, a price slower than the real one pays whoever follows it. Each
probe (the `probe` agent: a maker at a set width, a trend follower, a reversal trader or a flow
follower, trading a lot at a time) runs alone in the market, one family's settings at a time,
under many seeds. What it makes is valued at the true value where the market has one, at
settlement for a contract that settles, after fees; a family's best on the first seeds is then
judged on fresh ones, so that picking the luckiest of many does not count as an edge. A probe
whose interval on the fresh seeds lies above zero is flagged: the market pays it, and unless the
real market is shown to pay it too, the fit has an edge the real one lacks. Given several
scenarios, or a directory of them such as a twin fit's `twins/`, the seeds take them in turn.

Beside the fixed rules, a small learner is trained in the market itself and judged the same way
(learner.py): a fitted market can pay what no single rule takes but a learner finds, such as how
slowly its price follows the value, read from several signals at once. `--no-learner` leaves it
out; it needs `crowdbook_learn`, the engine's Python module.
"""

import argparse
import json
import math
import os
import subprocess
import tempfile
import tomllib
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from crowdbook_analysis import learner as learning
from crowdbook_analysis.runner import find_tool, pnl_at_value

SEARCH_SEED = 500_001
CONFIRM_SEED = 600_001


@dataclass(frozen=True)
class Probe:
    """One probe's settings, as the scenario's `probe` agent takes them."""

    family: str
    settings: tuple = ()  # (key, value) pairs, so that a probe can name a result

    @staticmethod
    def of(family: str, settings: dict) -> "Probe":
        return Probe(family, tuple(settings.items()))

    @property
    def name(self) -> str:
        shown = ", ".join(f"{k} {v}" for k, v in self.settings)
        return f"{self.family} ({shown})"

    def section(self, latency: str, size: int, max_position: int) -> str:
        lines = ["", "[[agents]]", 'type = "probe"', 'name = "probe"',
                 f'style = "{self.family}"', f"size = {size}", f"max_position = {max_position}"]
        for key, value in self.settings:
            lines.append(f'{key} = "{value}"' if isinstance(value, str) else f"{key} = {value}")
        lines.append(f'latency = {{ to_exchange = "{latency}", from_exchange = "{latency}" }}')
        lines.append(f"account = {{ max_position = {max_position + size}, "
                     f"max_order_quantity = {size} }}")
        return "\n".join(lines) + "\n"


def default_probes() -> list[Probe]:
    """Makers from a tick inside the usual touch to well outside it; trend and reversal traders
    over seconds to minutes; flow followers over short and long windows."""
    probes = [Probe.of("maker", {"offset": offset, "interval": "1s"})
              for offset in (0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0)]
    for family in ("trend", "reversal"):
        for fast, slow in (("2s", "20s"), ("10s", "60s"), ("60s", "600s")):
            for threshold in (0.5, 2.0):
                probes.append(Probe.of(family, {"fast_half_life": fast, "slow_half_life": slow,
                                             "threshold": threshold, "interval": "1s"}))
    for window in ("5s", "30s", "120s"):
        for imbalance in (0.6, 0.8):
            probes.append(Probe.of("flow", {"window": window, "imbalance": imbalance,
                                         "interval": "1s"}))
    return probes


def made(result: dict) -> tuple[float, int]:
    """What the probe made after fees, in tick-lots, valued at the true value or settlement where
    the run has one and at the last price otherwise, and the lots it traded."""
    for group in result["groups"]:
        if group["name"] == "probe":
            value = result.get("final_value")
            value = result["last_price"] if value is None else value
            return pnl_at_value(group, value), int(group["traded"])
    raise KeyError("probe")


def run_probe(tool: Path, scenario: str | list[str], probe: Probe, seeds, *, latency: str,
              size: int, max_position: int) -> np.ndarray:
    """What the probe made in each market, and what it traded: one row a seed. Given several
    scenarios, the i-th seed runs the (i mod n)-th."""
    scenarios = [scenario] if isinstance(scenario, str) else scenario
    section = probe.section(latency, size, max_position)
    seeds = list(seeds)
    with tempfile.TemporaryDirectory() as scratch:
        paths = []
        for index, text in enumerate(scenarios):
            paths.append(Path(scratch) / f"scenario{index}.toml")
            paths[-1].write_text(text + section)

        def one(index: int) -> tuple[float, int]:
            seed, path = seeds[index], paths[index % len(paths)]
            out = Path(scratch) / f"{index}.json"
            finished = subprocess.run([str(tool), "run", str(path), "--seed", str(seed),
                                       "--json", str(out)], capture_output=True, text=True)
            if finished.returncode != 0:
                raise RuntimeError(f"{probe.name}, seed {seed}: {finished.stderr.strip()}")
            return made(json.loads(out.read_text()))

        with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            return np.array(list(pool.map(one, range(len(seeds)))), dtype=float)


def summary(rows: np.ndarray) -> dict:
    """Mean made a market with a 95% interval, and per lot traded."""
    pnl, lots = rows[:, 0], rows[:, 1]
    mean = float(pnl.mean())
    error = float(pnl.std(ddof=1) / math.sqrt(len(pnl))) if len(pnl) > 1 else math.nan
    return {"mean": mean, "low": mean - 1.96 * error, "high": mean + 1.96 * error,
            "per_lot": float(pnl.sum() / lots.sum()) if lots.sum() > 0 else math.nan,
            "lots": float(lots.mean()), "markets": len(pnl)}


def sweep(tool: Path, scenario: str | list[str], probes: list[Probe], search: int, confirm: int, *,
          latency: str = "1ms", size: int = 1, max_position: int = 10,
          learner: learning.Settings | None = None, log=print) -> dict:
    """Every probe on the search seeds; each family's best on the confirming seeds. With
    `learner`, the learning probe too, trained at the same seat and judged on as many fresh
    markets; its own settings of latency, size and position are replaced by the sweep's."""
    found = {}
    for probe in probes:
        found[probe] = summary(run_probe(tool, scenario, probe,
                                         range(SEARCH_SEED, SEARCH_SEED + search),
                                         latency=latency, size=size, max_position=max_position))
        log(f"{probe.name}: {found[probe]['mean']:+.1f} a market")
    judged = {}
    for family in dict.fromkeys(p.family for p in probes):
        best = max((p for p in probes if p.family == family), key=lambda p: found[p]["mean"])
        judged[best] = summary(run_probe(tool, scenario, best,
                                         range(CONFIRM_SEED, CONFIRM_SEED + confirm),
                                         latency=latency, size=size, max_position=max_position))
    results = {"searched": found, "judged": judged}
    if learner is not None:
        results.update(learn(scenario, learner, confirm, latency=latency, size=size,
                             max_position=max_position, log=log))
        judged.update(results.pop("learner_judged"))
    return results


def learn(scenario: str | list[str], settings: learning.Settings, confirm: int, *, latency: str,
          size: int, max_position: int, log=print) -> dict:
    """The learning probe trained in the scenarios and judged on `confirm` fresh markets: its
    judged summary keyed by a probe that names it, its weights and its training."""
    settings = learning.Settings(**{**settings.__dict__, "latency": latency, "size": size,
                                    "max_position": max_position})
    texts = [learning.seat(text, latency=latency, size=size, max_position=max_position,
                           interval=settings.interval)
             for text in ([scenario] if isinstance(scenario, str) else scenario)]
    module = learning.learn_module()
    weights, history = learning.train(module, texts, settings, log=log)
    judged = learning.summary(learning.judge(module, texts, weights, confirm, settings))
    log(f"learner, on fresh markets: {judged['mean']:+.1f} a market")
    probe = Probe.of("learner", {"generations": settings.generations,
                                 "markets": settings.markets})
    return {"learner_judged": {probe: judged},
            "learner": {"weights": weights, "history": history, "settings": settings}}


def _cell(s: dict) -> str:
    return f"{s['mean']:+,.1f} [{s['low']:+,.1f}, {s['high']:+,.1f}]"


def report(name: str, results: dict, search: int, confirm: int) -> tuple[str, bool]:
    """The sweep as Markdown, and whether any probe made money on the confirming seeds."""
    lines = [f"# No-false-edge check: {name}", "",
             f"Each family's best on {search} markets, judged on {confirm} fresh ones; tick-lots "
             "a market after fees, with 95% intervals. A probe is flagged when the whole "
             "interval is above zero.", "",
             "| each family's best, on fresh markets | made a market | per lot traded | "
             "lots a market | flagged |", "|---|---:|---:|---:|---|"]
    flagged = False
    for probe, s in results["judged"].items():
        edge = s["low"] > 0
        flagged |= edge
        lines.append(f"| {probe.name} | {_cell(s)} | {s['per_lot']:+.3f} | {s['lots']:.1f} | "
                     f"{'**edge**' if edge else ''} |")
    lines += ["", f"**{'An edge: the market pays a strategy that should make nothing.' if flagged else 'No probe makes money after fees.'}**",
              "", "## Every probe, on the search's markets", "",
              "| probe | made a market | per lot traded | lots a market |", "|---|---:|---:|---:|"]
    for probe, s in results["searched"].items():
        lines.append(f"| {probe.name} | {_cell(s)} | {s['per_lot']:+.3f} | {s['lots']:.1f} |")
    if "learner" in results:
        lines += ["", *learner_section(results["learner"])]
    return "\n".join(lines) + "\n", flagged


def learner_section(trained: dict) -> list[str]:
    """How the learner trained, and the weights it trades on, as Markdown lines."""
    settings, history = trained["settings"], trained["history"]
    shown = ", ".join(f"{made:+.1f}" for made in history)
    return ["## The learner", "",
            f"Trained by CMA-ES for {settings.generations} generations, each candidate on "
            f"{settings.markets} markets, from weights of 0, a strategy that never trades, on "
            "what it made to its last decision, valued at the true value where there is one. "
            f"What each generation's candidates made a market so, on average: {shown}.", "",
            "Its weights: a take toward its limit on the side the target's score is past 1 on, "
            "and a bid or an offer at the best price where theirs is above 0. Signals with large "
            "weights are what the market pays it for.", "",
            learning.weights_table(trained["weights"]).rstrip()]


def scenario_files(paths: list[Path]) -> list[Path]:
    """The scenario files given, a directory standing for the .toml files in it, in name order."""
    files = []
    for path in paths:
        if not path.exists():
            raise SystemExit(f"no such scenario file or directory: {path}")
        files += sorted(path.glob("*.toml")) if path.is_dir() else [path]
    if not files:
        raise SystemExit(f"no scenario files in {', '.join(map(str, paths))}")
    return files


def add_learner_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--no-learner", action="store_true",
                        help="leave out the learning probe")
    parser.add_argument("--learner-generations", type=int, default=learning.Settings.generations,
                        help="the learner's generations of training "
                             f"({learning.Settings.generations})")
    parser.add_argument("--learner-markets", type=int, default=learning.Settings.markets,
                        help="markets for each of its candidates, each generation "
                             f"({learning.Settings.markets})")


def learner_settings(args) -> learning.Settings | None:
    if args.no_learner:
        return None
    if args.learner_generations < 1 or args.learner_markets < 1:
        raise SystemExit("the learner needs at least a generation and a market")
    return learning.Settings(generations=args.learner_generations, markets=args.learner_markets)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scenario", type=Path, nargs="+",
                        help="scenario files, or directories of them, taken in turn")
    parser.add_argument("--seeds", type=int, default=200, help="markets for each probe (200)")
    parser.add_argument("--confirm", type=int, default=800,
                        help="fresh markets for each family's best (800)")
    parser.add_argument("--latency", default="1ms",
                        help="the probes' latency each way, as a duration (1ms)")
    parser.add_argument("--size", type=int, default=1, help="lots an order (1)")
    parser.add_argument("--max-position", type=int, default=10)
    parser.add_argument("--families", help="only these, such as maker,trend")
    add_learner_arguments(parser)
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--out", type=Path, help="write the Markdown here")
    args = parser.parse_args()
    files = scenario_files(args.scenario)
    scenarios = [f.read_text() for f in files]
    for text in scenarios:
        tomllib.loads(text)  # a clear error for a file that is not a scenario
    probes = default_probes()
    if args.families:
        wanted = set(args.families.split(","))
        probes = [p for p in probes if p.family in wanted]
    results = sweep(find_tool(args.crowdbook), scenarios, probes, args.seeds, args.confirm,
                    latency=args.latency, size=args.size, max_position=args.max_position,
                    learner=learner_settings(args))
    name = files[0].name if len(files) == 1 else f"{len(files)} scenarios from {args.scenario[0]}"
    text, flagged = report(name, results, args.seeds, args.confirm)
    if args.out:
        args.out.write_text(text)
    print(text)
    raise SystemExit(1 if flagged else 0)


if __name__ == "__main__":
    main()
