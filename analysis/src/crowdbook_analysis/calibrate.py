"""Fits any scenario to a recorded market: the settings whose synthetic markets come closest to the
recording on the validation report's statistics, with how sure the fit is.

    uv run --project analysis crowdbook-calibrate spec.toml --real REAL_TAPE
    uv run --project analysis crowdbook-calibrate spec.toml --truth noise_rate=0.02,maker=4

A spec names a scenario template and the settings to fit:

    template = "my_market.toml"     # a scenario, with ${name} wherever a setting goes
    statistics = ["spread_mean", "orders_per_minute"]   # optional: default, every one both have
    seeds = 60                      # markets each candidate is judged on, the same for all
    generations = 30
    held_out = 200                  # markets the fit is judged on afterwards, on other seeds

    [parameters.noise_rate]
    low = 0.001
    high = 0.05
    start = 0.01                    # optional: default, the middle of the range
    log = true                      # moves in proportion (the default where low > 0)
    whole = false                   # a whole number, such as a count of traders
    kind = "number"                 # or "duration", written as 250ms

The method is that of `fit`, made general: the method of simulated moments (McFadden, 1989), with
tolerances from resampling real and simulated markets, a joint search by CMA-ES on common seeds,
a verdict on seeds the search never used, and the sandwich formula for the settings' intervals.
`--truth` fits markets the engine made itself at known settings instead of a recording: a fit
that cannot find settings it knows the answer to should not be trusted with ones it does not.

It writes the fitted scenario, the validation report of the start and the fit against the target,
and the settings with their intervals, as Markdown and as JSON.
"""

import argparse
import json
import math
import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from crowdbook_analysis import tape as tapes
from crowdbook_analysis import validate
from crowdbook_analysis.fit import Cma
from crowdbook_analysis.runner import find_tool

# A setting's place in a template; comment lines are left as they are.
PLACEHOLDER = re.compile(r"\$\{(\w+)\}")
FIT_SEED = 1
HELD_OUT_SEED = 100_001
JACOBIAN_SEED = 200_001
TRUTH_SEED = 900_001
BOOTSTRAP = 100


@dataclass(frozen=True)
class Parameter:
    name: str
    low: float
    high: float
    start: float
    log: bool = True
    whole: bool = False
    kind: str = "number"

    def to_unit(self, value: float) -> float:
        if self.log:
            return math.log(value / self.low) / math.log(self.high / self.low)
        return (value - self.low) / (self.high - self.low)

    def at(self, u: float) -> float:
        """The setting at a point of the unit box, clipped to it, not rounded."""
        u = min(max(u, 0.0), 1.0)
        return (self.low * (self.high / self.low) ** u if self.log
                else self.low + (self.high - self.low) * u)

    def from_unit(self, u: float) -> float:
        """The setting at a point of the unit box, as a scenario takes it: whole numbers rounded,
        durations to the microsecond."""
        value = self.at(u)
        if self.whole:
            return float(round(value))
        return round(value, 6) if self.kind == "duration" else value

    def text(self, value: float) -> str:
        if self.kind == "duration":
            micros = round(value * 1e6)
            if micros % 1_000_000 == 0:
                return f'"{micros // 1_000_000}s"'
            return f'"{micros // 1000}ms"' if micros % 1000 == 0 else f'"{micros}us"'
        if self.whole:
            return str(int(round(value)))
        return f"{value:.6g}"


@dataclass
class Spec:
    template: str
    parameters: list[Parameter]
    statistics: list[str] | None = None
    seeds: int = 60
    generations: int = 30
    held_out: int = 200

    def scenario(self, values: dict[str, float]) -> str:
        texts = {p.name: p.text(values[p.name]) for p in self.parameters}

        def fill(match: re.Match) -> str:
            if match.group(1) not in texts:
                raise KeyError(f"the template uses ${{{match.group(1)}}}, which the spec does "
                               "not set")
            return texts[match.group(1)]

        return "".join(line if line.lstrip().startswith("#") else PLACEHOLDER.sub(fill, line)
                       for line in self.template.splitlines(keepends=True))

    def start(self) -> dict[str, float]:
        return {p.name: p.start for p in self.parameters}

    def to_unit(self, values: dict[str, float]) -> np.ndarray:
        return np.array([p.to_unit(values[p.name]) for p in self.parameters])

    def from_unit(self, point: np.ndarray) -> dict[str, float]:
        return {p.name: p.from_unit(u) for p, u in zip(self.parameters, point, strict=True)}


def read_spec(path: Path) -> Spec:
    spec = tomllib.loads(path.read_text())
    template = (path.parent / spec["template"]).read_text()
    parameters = []
    for name, entry in spec.get("parameters", {}).items():
        low, high = float(entry["low"]), float(entry["high"])
        if not low < high:
            raise ValueError(f"parameter {name}: low must be below high")
        log = bool(entry.get("log", low > 0))
        if log and low <= 0:
            raise ValueError(f"parameter {name}: a logarithmic range needs low > 0")
        start = float(entry.get("start", math.sqrt(low * high) if log else (low + high) / 2))
        parameters.append(Parameter(name, low, high, start, log, bool(entry.get("whole", False)),
                                    entry.get("kind", "number")))
    if not parameters:
        raise ValueError("the spec sets no [parameters]")
    used = {name for line in template.splitlines() if not line.lstrip().startswith("#")
            for name in PLACEHOLDER.findall(line)}
    unused = {p.name for p in parameters} - used
    if unused:
        raise ValueError(f"the template never uses {', '.join(sorted(unused))}")
    return Spec(template, parameters, spec.get("statistics"), int(spec.get("seeds", 60)),
                int(spec.get("generations", 30)), int(spec.get("held_out", 200)))


class Target:
    """What the fit aims at: the real market's statistics, the ones compared, and their
    tolerances."""

    def __init__(self, real: validate.Summary, keys: list[str], logs: list[bool],
                 tolerance: np.ndarray):
        self.real, self.keys, self.logs, self.tolerance = real, keys, logs, tolerance
        self.vector = self.scaled(real.point)

    def scaled(self, point: dict[str, float]) -> np.ndarray:
        return np.array([float(validate._scaled(point.get(k, np.nan), log))
                         for k, log in zip(self.keys, self.logs, strict=True)])

    def distance(self, point: dict[str, float]) -> float:
        gaps = (self.scaled(point) - self.vector) / self.tolerance
        return float(np.sum(np.where(np.isfinite(gaps), gaps**2, 1e6)))


class Evaluator:
    """Simulates settings and measures them, counting every market run."""

    def __init__(self, tool: Path, spec: Spec, config: validate.Config):
        self.tool, self.spec, self.config, self.runs = tool, spec, config, 0

    def tape(self, values: dict[str, float], seeds) -> tapes.Tape:
        self.runs += len(seeds)
        return tapes.simulate(self.tool, self.spec.scenario(values), seeds,
                              labels={k: float(v) for k, v in values.items()})

    def point(self, values: dict[str, float], seeds) -> dict[str, float]:
        t = self.tape(values, seeds)
        ps = validate.pieces(t, self.config)
        for p in ps:
            p["tick"] = t.options.tick
        return validate.measure(ps, self.config)

    def summary(self, name: str, values: dict[str, float], seeds, seed: int) -> validate.Summary:
        return validate.summarize(name, self.tape(values, seeds), self.config, seed)


def make_target(real: validate.Summary, start: validate.Summary, wanted: list[str] | None,
                config: validate.Config) -> Target:
    """The statistics both sides have, each with a tolerance from the two resampling spreads."""
    keys, logs, tolerance = [], [], []
    for key, _, log in validate.statistic_names(config):
        if wanted is not None and key not in wanted:
            continue
        if not (np.isfinite(real.point.get(key, np.nan))
                and np.isfinite(start.point.get(key, np.nan))):
            continue
        spread = math.hypot(validate.spread_of(real.draws[key], log),
                            validate.spread_of(start.draws[key], log))
        if np.isfinite(spread) and spread > 0:
            keys.append(key)
            logs.append(log)
            tolerance.append(spread)
    if wanted is not None and set(wanted) - set(keys):
        print(f"left out, as one side cannot measure them: {sorted(set(wanted) - set(keys))}")
    return Target(real, keys, logs, np.array(tolerance))


def search(evaluator: Evaluator, target: Target, seeds, generations: int,
           log=print) -> tuple[dict[str, float], float]:
    spec = evaluator.spec
    start = spec.start()
    best, best_score = start, target.distance(evaluator.point(start, seeds))
    log(f"start: {best_score:.1f}")
    cma = Cma(spec.to_unit(start), sigma=0.15, seed=1)
    for generation in range(generations):
        points = cma.ask()
        scores = []
        for point in points:
            values = spec.from_unit(point)
            outside = float(np.sum((point - np.clip(point, 0.0, 1.0)) ** 2))
            score = target.distance(evaluator.point(values, seeds))
            scores.append(score + 1e4 * outside)
            if score < best_score:
                best, best_score = values, score
        cma.tell(points, np.array(scores))
        log(f"generation {generation + 1}: best {min(scores):.1f}, overall {best_score:.1f}, "
            f"step {cma.sigma:.3f}")
    return best, best_score


def derivatives(evaluator: Evaluator, target: Target, values: dict[str, float],
                seeds) -> np.ndarray:
    """How each compared statistic moves with each setting in the unit box, by central
    differences on common seeds; a whole number steps to its neighbours."""
    spec = evaluator.spec
    center = spec.to_unit(values)
    columns = []
    for index, p in enumerate(spec.parameters):
        high, low = center.copy(), center.copy()
        if p.whole:
            high[index] = p.to_unit(min(values[p.name] + 1, p.high))
            low[index] = p.to_unit(max(values[p.name] - 1, p.low))
        else:
            high[index] = min(center[index] + 0.05, 1.0)
            low[index] = max(center[index] - 0.05, 0.0)
        if high[index] == low[index]:
            columns.append(np.zeros(len(target.keys)))
            continue
        above = target.scaled(evaluator.point(spec.from_unit(high), seeds))
        below = target.scaled(evaluator.point(spec.from_unit(low), seeds))
        columns.append((above - below) / (high[index] - low[index]))
    return np.nan_to_num(np.column_stack(columns))


def covariance_of(summaries: list[validate.Summary], target: Target) -> np.ndarray:
    """The covariance of the compared statistics' resamples, summed over the summaries."""
    total = np.zeros((len(target.keys), len(target.keys)))
    for s in summaries:
        draws = np.column_stack([validate._scaled(s.draws[k], log)
                                 for k, log in zip(target.keys, target.logs, strict=True)])
        draws = draws[np.all(np.isfinite(draws), axis=1)]
        if len(draws) > 2:
            total += np.cov(draws, rowvar=False)
    return total


def intervals(spec: Spec, fitted: dict[str, float], covariance: np.ndarray,
              jacobian: np.ndarray) -> list[dict]:
    """Each setting's 68% interval from its standard error in the unit box, mapped back; with
    the same cautions as `fit.settings_table`."""
    center = spec.to_unit(fitted)
    errors = np.sqrt(np.maximum(np.diag(covariance), 0.0))
    reach = np.linalg.norm(jacobian, axis=0)
    rows = []
    for index, p in enumerate(spec.parameters):
        row = {"name": p.name, "start": p.start, "fitted": fitted[p.name], "low": None,
               "high": None, "note": ""}
        if reach[index] <= 1e-9 * max(reach.max(), 1e-300):
            row["note"] = "not pinned down: no statistic moves with it here"
        elif center[index] <= 0.01 or center[index] >= 0.99:
            row["note"] = "at the edge of its range"
        elif errors[index] >= 0.5:
            row["note"] = "not pinned down"
        else:
            # Whole numbers get intervals as if they could take any value between.
            row["low"] = p.at(center[index] - errors[index])
            row["high"] = p.at(center[index] + errors[index])
            if center[index] - errors[index] < 0 or center[index] + errors[index] > 1:
                row["note"] = "cut short by the range's edge"
        rows.append(row)
    return rows


def settings_table(rows: list[dict], truth: dict[str, float] | None) -> str:
    head = "| setting | start | fitted | 68% interval |" + (" truth | inside |" if truth else "")
    lines = [head, "|---|---:|---:|---:|" + ("---:|---:|" if truth else "")]
    for row in rows:
        bounds = (f"{row['low']:.3g} to {row['high']:.3g}" if row["low"] is not None else "")
        if row["note"]:
            bounds = f"{bounds}, {row['note']}" if bounds else row["note"]
        line = f"| {row['name']} | {row['start']:.3g} | {row['fitted']:.3g} | {bounds} |"
        if truth:
            value = truth[row["name"]]
            inside = (row["low"] is not None and row["low"] <= value <= row["high"])
            line += f" {value:.3g} | {'yes' if inside else 'no'} |"
        lines.append(line)
    return "\n".join(lines) + "\n"


def chi_squared_95(degrees: int) -> float:
    """The 95th percentile of chi-squared, by Wilson and Hilferty's cube-root approximation."""
    k = max(degrees, 1)
    return k * (1 - 2 / (9 * k) + 1.6449 * math.sqrt(2 / (9 * k))) ** 3


def parse_truth(text: str, spec: Spec) -> dict[str, float]:
    values = spec.start()
    for item in filter(None, text.split(",")):
        name, _, value = item.partition("=")
        if name not in values:
            raise SystemExit(f"--truth: {name} is not a parameter of the spec")
        values[name] = float(value)
    return values


def calibrate(spec: Spec, evaluator: Evaluator, real: validate.Summary, *,
              truth: dict[str, float] | None = None, log=print) -> tuple[str, dict, str]:
    """The whole fit: returns the report as Markdown, as a dictionary, and the fitted scenario."""
    config = evaluator.config
    fit_seeds = range(FIT_SEED, FIT_SEED + spec.seeds)
    held_out = range(HELD_OUT_SEED, HELD_OUT_SEED + spec.held_out)
    start_summary = evaluator.summary("start", spec.start(), fit_seeds, seed=2)
    target = make_target(real, start_summary, spec.statistics, config)
    log(f"{real.markets} target markets; {len(target.keys)} statistics compared")
    fitted, score = search(evaluator, target, fit_seeds, spec.generations, log)
    log(f"fitted, distance {score:.1f} on the fit's seeds")
    jacobian = derivatives(evaluator, target, fitted,
                           range(JACOBIAN_SEED, JACOBIAN_SEED + max(spec.seeds, 100)))
    fitted_fit_seeds = evaluator.summary("fitted", fitted, fit_seeds, seed=3)
    weight = np.diag(1 / target.tolerance**2)
    bread = np.linalg.pinv(jacobian.T @ weight @ jacobian)
    meat = jacobian.T @ weight @ covariance_of([real, fitted_fit_seeds], target) @ weight \
        @ jacobian
    rows = intervals(spec, fitted, bread @ meat @ bread, jacobian)
    judged = [evaluator.summary("start", spec.start(), held_out, seed=4),
              evaluator.summary("fitted", fitted, held_out, seed=5)]
    text, data = validate.report(real, judged, config)
    lines = [f"# Calibration against {real.name}", "",
             f"{len(spec.parameters)} settings fitted to {len(target.keys)} statistics by CMA-ES, "
             f"{spec.generations} generations on {spec.seeds} common seeds; distance "
             f"{score:.1f} on those seeds. Judged below on {spec.held_out} seeds the search "
             f"never used. {evaluator.runs} markets run in all.", "",
             verdict, "", "## Settings", "", settings_table(rows, truth), "",
             "68% intervals from the sandwich formula of simulated moments: they count sampling "
             "error and the simulation's noise, not the ways the model differs from the market.",
             "", "## Held-out seeds", "", text.split("\n", 2)[2]]
    held_distance = target.distance(judged[1].point)
    # At the true settings each compared statistic is off by chance alone, so the distance is
    # about chi-squared with one degree of freedom a statistic, or less on the held-out seeds,
    # which are more than the fit's. Beyond its 95th percentile the model does not reach the
    # target, and the sandwich formula, which assumes it does, says too little.
    reached = held_distance <= chi_squared_95(len(target.keys))
    verdict = (f"Distance on the held-out seeds: {held_distance:.1f}, against "
               f"{chi_squared_95(len(target.keys)):.1f} that chance alone would stay under 95% of "
               "the time. " + ("The fit reaches the target within chance." if reached else
                               "**The fit does not reach the target within chance**: the model, "
                               "or the search, falls short, and the intervals below are too "
                               "narrow to trust."))
    data["calibration"] = {"settings": rows, "distance_fit_seeds": score,
                           "distance_held_out": held_distance, "reached": reached,
                           "compared": target.keys,
                           "runs": evaluator.runs, "truth": truth}
    return "\n".join(lines), data, spec.scenario(fitted)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path)
    parser.add_argument("--real", type=Path, help="the recorded market's tape")
    parser.add_argument("--truth", help="fit the engine's own markets made at these settings, "
                        "name=value,... (the rest at their starts)")
    parser.add_argument("--truth-markets", type=int, default=400)
    parser.add_argument("--generations", type=int)
    parser.add_argument("--seeds", type=int)
    parser.add_argument("--held-out", type=int)
    parser.add_argument("--bootstrap", type=int, default=BOOTSTRAP)
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--output", type=Path, default=Path("calibrated"),
                        help="directory for the fitted scenario and the report")
    args = parser.parse_args()
    if bool(args.real) == bool(args.truth):
        parser.error("give one of --real or --truth")
    spec = read_spec(args.spec)
    for name in ("generations", "seeds", "held_out"):
        if getattr(args, name) is not None:
            setattr(spec, name, getattr(args, name))
    config = validate.Config(bootstrap=args.bootstrap)
    evaluator = Evaluator(find_tool(args.crowdbook), spec, config)
    truth = None
    if args.truth:
        truth = parse_truth(args.truth, spec)
        seeds = range(TRUTH_SEED, TRUTH_SEED + args.truth_markets)
        real = validate.summarize("truth", evaluator.tape(truth, seeds), config, seed=1)
    else:
        real = validate.summarize(args.real.name, tapes.read_tape(args.real), config, seed=1)
    text, data, scenario = calibrate(spec, evaluator, real, truth=truth,
                                     log=lambda line: print(line, flush=True))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "scenario.toml").write_text(scenario)
    (args.output / "report.md").write_text(text)
    (args.output / "report.json").write_text(json.dumps(data, indent=1, default=float))
    print(text)


if __name__ == "__main__":
    main()
