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
    window = [0, 1800]              # optional: compare only this part of each market, in seconds
    phase = "from kickoff"          # optional: or the part of each from (or "before") a label
    sample_interval = 0.1           # optional: seconds between samples of the simulated book
    condition = true                # optional: twins of each target market (see below)

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
With `condition = true`, each market of the target gets synthetic twins of its own: the template
may use `${duration}`, its length in seconds, any of its labels by name (such as `${kickoff}`),
for a binary contract `${opening_probability}`, where it opened, `${value_path}`, its reference
value as a `[prediction]` path, and `${outcome}`, what it settled at (where its value ended if it
has not settled), and for a game `${script}`, its scores replayed (see
`[prediction.game]`); `seeds` and
`held_out` then count the twins of each market. Markets alike in what is known about them are
compared with each other, and the luck of a game's result is taken out of the comparison.

`--truth` fits markets the engine made itself at known settings instead of a recording: a fit
that cannot find settings it knows the answer to should not be trusted with ones it does not.

It writes the fitted scenario, the validation report of the start and the fit against the target,
and the settings with their intervals, as Markdown and as JSON; with `condition = true`, also
`twins/`, the fitted twin of every recorded market, which `crowdbook-sweep` and the strategy
harness take as they are.
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
from crowdbook_analysis.runner import Run, find_tool

# A setting's place in a template; comment lines are left as they are.
# `${name:default}` gives what to write where nothing sets it, as for a label some recordings lack.
PLACEHOLDER = re.compile(r"\$\{(\w+)(?::([^}]*))?\}")
FIT_SEED = 1
HELD_OUT_SEED = 100_001
JACOBIAN_SEED = 200_001
REFIT_SEED = 1_000_001
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
    # The part of each market compared, from and to seconds (to its end where None), or all of it.
    window: tuple[float, float | None] | None = None
    sample_interval: float = tapes.SAMPLE_INTERVAL  # seconds between samples of the book
    # Each target market gets synthetic twins of its own, made from the template with its
    # duration, labels and scores; `seeds` and `held_out` then count twins of each.
    condition: bool = False
    # The part of each market compared, cut at a time a label of its own gives, such as a game's
    # kickoff: (label, True) from it on, (label, False) before it; or all of it.
    phase: tuple[str, bool] | None = None

    def cut(self, t: tapes.Tape) -> tapes.Tape:
        if self.window is not None:
            t = tapes.window(t, *self.window)
        if self.phase is not None:
            t = tapes.phase(t, self.phase[0], self.phase[1])
        return t

    def scenario(self, values: dict[str, float], market: tapes.Market | None = None) -> str:
        texts = {p.name: p.text(values[p.name]) for p in self.parameters}
        if market is not None:
            texts = market_texts(market) | texts

        def fill(match: re.Match) -> str:
            if match.group(1) not in texts and match.group(2) is not None:
                return match.group(2)
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


DRAW_HIGHEST = 0.26


def value_path(m: tapes.Market, payout: float) -> list[tuple[float, float]]:
    """A market's recorded reference value as a prediction market's `path`: (seconds, cents)
    pairs, from its start on, where the value changed."""
    if m.value is None or m.value_time is None:
        return []
    path: list[tuple[float, float]] = []
    for time, value in zip(m.value_time, m.value, strict=True):
        if not np.isfinite(value) or not np.isfinite(time):
            continue
        cents = round(float(np.clip(value * 100.0 / payout, 0.0, 100.0)), 4)
        if not path or cents != path[-1][1]:
            path.append((round(max(float(time), 0.0), 6), cents))
    return path


def with_openings(t: tapes.Tape, low: float = 0.05, high: float = 0.9) -> list[tapes.Market]:
    """A binary contract's markets, each labelled `opening_probability`: its first reference
    value, else its first mid, as a share of the payout, kept from `low` to `high`, so that its
    twins open where it did. A game whose draw pays nothing cannot start its home side at much
    more than 90%, so a heavier favourite is started there. Each is labelled too with
    `value_path`, its reference value as (seconds, cents) pairs, and `outcome`, what it settled
    at in cents, or where its reference value ended for a market not yet settled, where the tape
    has them. A `kickoff` label outside the market's run is moved to its nearer end.
    """
    if not t.options.payout:
        return t.markets
    for m in t.markets:
        # A twin's kickoff falls within its run: a game already under way when the recording
        # began kicks off at its opening, and one after the recording ended at its close. The
        # recorded label is kept as `kickoff_recorded`.
        kickoff = m.labels.get("kickoff")
        if isinstance(kickoff, (int, float)) and not isinstance(kickoff, bool) \
                and np.isfinite(kickoff) and not 0.0 <= kickoff <= m.duration:
            m.labels["kickoff_recorded"] = kickoff
            m.labels["kickoff"] = float(np.clip(kickoff, 0.0, m.duration))
        m.labels["value_path"] = value_path(m, t.options.payout)
        if m.outcome is not None and np.isfinite(m.outcome):
            m.labels["outcome"] = float(np.clip(m.outcome * 100.0 / t.options.payout, 0, 100))
        elif m.labels["value_path"]:
            m.labels["outcome"] = m.labels["value_path"][-1][1]
        first = np.nan
        if m.value is not None and len(m.value):
            first = m.value[np.isfinite(m.value)][0] if np.isfinite(m.value).any() else np.nan
        if not np.isfinite(first):
            mids = (m.bid_price[:, 0] + m.ask_price[:, 0]) / 2
            mids = mids[np.isfinite(mids)]
            first = mids[0] if len(mids) else np.nan
        if np.isfinite(first):
            # A contract on a draw cannot open above the chance of a draw in an even game, about
            # 27% with the 2.5 scores the game twins expect at least.
            top = DRAW_HIGHEST if m.labels.get("side") == "draw" else high
            m.labels["opening_probability"] = float(np.clip(first / t.options.payout, low, top))
    return t.markets


def _plain(value: float) -> str:
    """A number as a scenario file and a duration both read it: positional, never with an
    exponent, to a millionth, so that seconds are never finer than the engine's clock."""
    return np.format_float_positional(round(float(value), 6), trim="-")


def market_texts(market: tapes.Market) -> dict[str, str]:
    """What a target market fills into a template for its twins: `${duration}` in seconds, each
    label by its name, numbers as they are and text in quotes, and `${script}`, its scores as a
    game's script, every change of the score from the market's start on."""
    texts = {"duration": _plain(market.duration)}
    for name, value in market.labels.items():
        if isinstance(value, bool) or value is None:
            continue
        if isinstance(value, list):
            texts[name] = "[" + ", ".join(
                "[" + ", ".join(_plain(x) for x in row) + "]" for row in value) + "]"
            continue
        if isinstance(value, (int, float)):
            if np.isfinite(value):
                texts[name] = _plain(value)
        else:
            texts[name] = json.dumps(str(value))
    entries = []
    if market.scores is not None:
        home = away = 0.0
        for time, now_home, now_away in market.scores:
            at = f'"{max(time, 0.0):.3f}s"'
            for side, gained in (("home", now_home - home), ("away", now_away - away)):
                if gained >= 1:
                    entries.append(f'{{ at = {at}, side = "{side}", points = {int(gained)} }}')
            home, away = max(home, now_home), max(away, now_away)
    texts["script"] = "[" + ", ".join(entries) + "]"
    return texts


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
    found = [match for line in template.splitlines() if not line.lstrip().startswith("#")
             for match in PLACEHOLDER.finditer(line)]
    used = {match.group(1) for match in found}
    unknown = {match.group(1) for match in found if match.group(2) is None} - {
        p.name for p in parameters}
    if unknown and not spec.get("condition", False):
        raise ValueError(f"the template uses {', '.join(sorted(unknown))}, which the spec does "
                         "not set; a target market's own values need condition = true")
    unused = {p.name for p in parameters} - used
    if unused:
        raise ValueError(f"the template never uses {', '.join(sorted(unused))}")
    window = None
    if "window" in spec:
        bounds = spec["window"]
        if not (isinstance(bounds, list) and 1 <= len(bounds) <= 2):
            raise ValueError("window is [start] or [start, end], in seconds")
        window = (float(bounds[0]), float(bounds[1]) if len(bounds) == 2 else None)
    phase = None
    if "phase" in spec:
        words = str(spec["phase"]).split()
        if len(words) != 2 or words[0] not in ("from", "before"):
            raise ValueError('phase is "from LABEL" or "before LABEL", such as "from kickoff"')
        phase = (words[1], words[0] == "from")
    return Spec(template, parameters, spec.get("statistics"), int(spec.get("seeds", 60)),
                int(spec.get("generations", 30)), int(spec.get("held_out", 200)), window,
                float(spec.get("sample_interval", tapes.SAMPLE_INTERVAL)),
                bool(spec.get("condition", False)), phase)


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


# At most this many markets run at once across candidates, which bounds the memory of their
# measurements; one candidate's markets always run together.
BATCH_RUNS = 256
# A market's samples that only the report's distributions and charts read, not its statistics.
UNMEASURED = ("spread_grid", "touch_grid", "returns_first")


class Evaluator:
    """Simulates settings and measures them, counting every market run."""

    def __init__(self, tool: Path, spec: Spec, config: validate.Config,
                 targets: list[tapes.Market] | None = None):
        """With `targets`, the markets a conditioned spec makes twins of."""
        self.tool, self.spec, self.config, self.runs = tool, spec, config, 0
        self.targets = targets or []
        if spec.condition and not self.targets:
            raise ValueError("a conditioned spec needs the target's markets")

    def _runs(self, values: dict[str, float], seeds) -> list[Run]:
        if not self.spec.condition:
            scenario = self.spec.scenario(values)
            return [Run(scenario, seed) for seed in seeds]
        # Twins: each seed once for each target market, its seed apart from every other's.
        scenarios = [self.spec.scenario(values, m) for m in self.targets]
        return [Run(scenario, seed * 1_000_003 + index)
                for seed in seeds for index, scenario in enumerate(scenarios)]

    def tape(self, values: dict[str, float], seeds) -> tapes.Tape:
        labels = {k: float(v) for k, v in values.items()}
        runs = self._runs(values, seeds)
        self.runs += len(runs)
        t = tapes.simulate_runs(self.tool, runs, labels=labels,
                                sample_interval=self.spec.sample_interval)
        for index, market in enumerate(t.markets):
            self._label_twin(index, market)
        return self.spec.cut(t)

    def _label_twin(self, index: int, market: tapes.Market) -> None:
        """A twin's run carries its recorded market's labels, such as its kickoff, so that it is
        cut where its recorded market is; the run's own labels win."""
        if self.spec.condition:
            target = self.targets[index % len(self.targets)]
            own = {k: v for k, v in target.labels.items() if not isinstance(v, list)}
            market.labels = own | market.labels

    def point(self, values: dict[str, float], seeds) -> dict[str, float]:
        return self.points([values], seeds)[0]

    def points(self, candidates: list[dict[str, float]], seeds) -> list[dict[str, float]]:
        """The statistics of each candidate's markets on these seeds. Candidates' markets run
        together, up to `BATCH_RUNS` at a time, each measured as it finishes, so that no core
        waits for one candidate's slowest market or for the measuring."""
        batches = [self._runs(values, seeds) for values in candidates]
        out: list[dict[str, float]] = []
        group: list[list[Run]] = []
        for batch in batches + [None]:
            if group and (batch is None or sum(map(len, group)) + len(batch) > BATCH_RUNS):
                out += self._measure_together(group)
                group = []
            if batch is not None:
                group.append(batch)
        return out

    def _measure_together(self, batches: list[list[Run]]) -> list[dict[str, float]]:
        runs = [run for batch in batches for run in batch]
        self.runs += len(runs)
        options = tapes.options_for(runs[0].scenario)

        def measured(index: int, market: tapes.Market) -> list[dict]:
            self._label_twin(index, market)
            cut = self.spec.cut(tapes.Tape([market], options)).markets
            pieces = [validate.market_pieces(m, options, self.config) for m in cut]
            for p in pieces:  # samples kept only for the report's distributions and charts
                for key in UNMEASURED:
                    p.pop(key, None)
            return pieces

        pieces = tapes.simulate_markets(self.tool, runs, then=measured,
                                        sample_interval=self.spec.sample_interval)
        out, at = [], 0
        for batch in batches:
            ps = [p for found in pieces[at:at + len(batch)] for p in found]
            at += len(batch)
            out.append(validate.measure(ps, self.config))
        return out

    def summary(self, name: str, values: dict[str, float], seeds, seed: int) -> validate.Summary:
        return validate.summarize(name, self.tape(values, seeds), self.config, seed)


def make_target(real: validate.Summary, start: validate.Summary, wanted: list[str] | None,
                config: validate.Config) -> Target:
    """The statistics both sides have, each with a tolerance from the two resampling spreads."""
    keys, logs, tolerance = [], [], []
    for key, _, log in validate.statistic_names(config):
        if wanted is not None and key not in wanted:
            continue
        # What a standing quote earns checks a fit for edges the real market lacks; fitted to,
        # it would check nothing.
        if wanted is None and key.startswith("quote_"):
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
           log=print, *, start: dict[str, float] | None = None, sigma: float = 0.15,
           seed: int = 1) -> tuple[dict[str, float], float]:
    spec = evaluator.spec
    start = spec.start() if start is None else start
    best, best_score = start, target.distance(evaluator.point(start, seeds))
    log(f"start: {best_score:.1f}")
    cma = Cma(spec.to_unit(start), sigma=sigma, seed=seed)
    for generation in range(generations):
        points = cma.ask()
        candidates = [spec.from_unit(point) for point in points]
        scores = []
        for point, values, measured in zip(points, candidates,
                                           evaluator.points(candidates, seeds), strict=True):
            outside = float(np.sum((point - np.clip(point, 0.0, 1.0)) ** 2))
            score = target.distance(measured)
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
    differences on common seeds, a step of 0.05 of the range either way; a whole number steps at
    least to its neighbours. A step of one alone, for a count of 20, moves the statistics by
    little more than their noise, and a noisy derivative makes the intervals too narrow."""
    spec = evaluator.spec
    center = spec.to_unit(values)
    steps = []  # (high, low) in the unit box, or None for a setting that cannot move
    for index, p in enumerate(spec.parameters):
        high, low = center.copy(), center.copy()
        high[index] = min(center[index] + 0.05, 1.0)
        low[index] = max(center[index] - 0.05, 0.0)
        if p.whole:
            above = max(p.from_unit(high[index]), min(values[p.name] + 1, p.high))
            below = min(p.from_unit(low[index]), max(values[p.name] - 1, p.low))
            high[index], low[index] = p.to_unit(above), p.to_unit(below)
        steps.append(None if high[index] == low[index] else (high, low))
    # Every step's markets as one batch.
    moved = [point for step in steps if step is not None for point in step]
    measured = iter(evaluator.points([spec.from_unit(point) for point in moved], seeds))
    columns = []
    for index, step in enumerate(steps):
        if step is None:
            columns.append(np.zeros(len(target.keys)))
            continue
        high, low = step
        above, below = target.scaled(next(measured)), target.scaled(next(measured))
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


def draw_settings(spec: Spec, rows: list[dict], count: int, *, seed: int = 1,
                  widen: float = 2.0) -> list[dict[str, float]]:
    """Settings drawn around a fit, as a report's rows give it: each setting normal in the unit
    box about its fitted value, with its 68% interval's half width, times `widen`, as the standard
    deviation, and kept within its range. A setting the fit could not pin down stays at its
    fitted value. `widen` is 2 by default since recovering known settings found the intervals
    about half as wide as they should be without refits (docs/validation.md); 1 suits intervals
    that count them."""
    rng = np.random.default_rng(seed)
    by_name = {row["name"]: row for row in rows}
    drawn = []
    for _ in range(count):
        values = {}
        for p in spec.parameters:
            row = by_name[p.name]
            center = p.to_unit(row["fitted"])
            if row.get("low") is None or row.get("high") is None:
                values[p.name] = row["fitted"]
                continue
            spread = widen * (p.to_unit(row["high"]) - p.to_unit(row["low"])) / 2
            values[p.name] = p.from_unit(center + spread * rng.standard_normal())
        drawn.append(values)
    return drawn


def draws_main() -> None:
    """crowdbook-draws: scenarios drawn from a fit's uncertainty, to judge a strategy across what
    the fit allows rather than at its point alone."""
    parser = argparse.ArgumentParser(description=draws_main.__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path)
    parser.add_argument("--report", type=Path, required=True,
                        help="the fit's report.json, from crowdbook-calibrate")
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--widen", type=float,
                        help="times the intervals' half widths (2, or 1 for a fit with --refits)")
    parser.add_argument("--out", type=Path, required=True, help="a directory for the scenarios")
    args = parser.parse_args()
    spec = read_spec(args.spec)
    if spec.condition:
        parser.error("a conditioned spec's scenarios are twins of recorded markets: draw "
                     "settings with calibrate.draw_settings and fill each twin")
    calibration = json.loads(args.report.read_text())["calibration"]
    rows = calibration["settings"]
    # Intervals that count the search's own noise need no widening.
    widen = args.widen if args.widen is not None else (
        1.0 if calibration.get("refits", 0) >= 2 else 2.0)
    args.out.mkdir(parents=True, exist_ok=True)
    for index, values in enumerate(draw_settings(spec, rows, args.count, seed=args.seed,
                                                 widen=widen)):
        header = "".join(f"# {k} = {v!r}\n" for k, v in values.items())
        (args.out / f"draw-{index:03d}.toml").write_text(
            f"# Drawn from the fit's uncertainty, draw {index}:\n{header}\n"
            + spec.scenario(values))
    print(f"{args.count} scenarios in {args.out}")


def parse_truth(text: str, spec: Spec) -> dict[str, float]:
    values = spec.start()
    for item in filter(None, text.split(",")):
        name, _, value = item.partition("=")
        if name not in values:
            raise SystemExit(f"--truth: {name} is not a parameter of the spec")
        values[name] = float(value)
    return values


def calibrate(spec: Spec, evaluator: Evaluator, real: validate.Summary, *,
              truth: dict[str, float] | None = None, log=print, refits: int = 0,
              refit_generations: int = 15) -> tuple[str, dict, str]:
    """The whole fit: returns the report as Markdown, as a dictionary, and the fitted scenario.
    With `refits` of 2 or more, the search runs again that many times from the fit, on fresh
    seeds, and the spread of where it lands is added to the intervals."""
    config = evaluator.config
    fit_seeds = range(FIT_SEED, FIT_SEED + spec.seeds)
    held_out = range(HELD_OUT_SEED, HELD_OUT_SEED + spec.held_out)
    start_summary = evaluator.summary("start", spec.start(), fit_seeds, seed=2)
    target = make_target(real, start_summary, spec.statistics, config)
    log(f"{real.markets} target markets; {len(target.keys)} statistics compared")
    fitted, score = search(evaluator, target, fit_seeds, spec.generations, log)
    log(f"fitted, distance {score:.1f} on the fit's seeds: {fitted}")
    # About 100 markets for each difference, or the fit's own number if more: with twins, that
    # many all told.
    per_seed = max(len(evaluator.targets), 1) if spec.condition else 1
    jacobian = derivatives(evaluator, target, fitted,
                           range(JACOBIAN_SEED, JACOBIAN_SEED + max(spec.seeds,
                                                                    math.ceil(100 / per_seed))))
    fitted_fit_seeds = evaluator.summary("fitted", fitted, fit_seeds, seed=3)
    weight = np.diag(1 / target.tolerance**2)
    bread = np.linalg.pinv(jacobian.T @ weight @ jacobian)
    meat = jacobian.T @ weight @ covariance_of([real, fitted_fit_seeds], target) @ weight \
        @ jacobian
    covariance = bread @ meat @ bread
    # The search's own noise: where it lands depends on the seeds it scores candidates on, which
    # the sandwich formula does not count. Searches again from the fit on fresh seeds measure it.
    refitted = []
    for refit in range(refits):
        seeds = range(REFIT_SEED + refit * 100_000, REFIT_SEED + refit * 100_000 + spec.seeds)
        again, _ = search(evaluator, target, seeds, refit_generations,
                          lambda line, r=refit: log(f"refit {r + 1}: {line}"),
                          start=fitted, sigma=0.05, seed=2 + refit)
        refitted.append(spec.to_unit(again))
    if len(refitted) >= 2:
        covariance = covariance + np.cov(np.array(refitted), rowvar=False)
    rows = intervals(spec, fitted, covariance, jacobian)
    judged = [evaluator.summary("start", spec.start(), held_out, seed=4),
              evaluator.summary("fitted", fitted, held_out, seed=5)]
    text, data = validate.report(real, judged, config)
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
    lines = [f"# Calibration against {real.name}", "",
             f"{len(spec.parameters)} settings fitted to {len(target.keys)} statistics by CMA-ES, "
             f"{spec.generations} generations on {spec.seeds} common seeds; distance "
             f"{score:.1f} on those seeds. Judged below on {spec.held_out} seeds the search "
             f"never used. {evaluator.runs} markets run in all.", "",
             verdict, "", "## Settings", "", settings_table(rows, truth), "",
             ("68% intervals from the sandwich formula of simulated moments, with the spread of "
              f"{refits} searches again on fresh seeds added: they count sampling error, the "
              "simulation's noise and the search's own, the simulation's noise in both parts, so "
              "they lean wide"
              if refits >= 2 else
              "68% intervals from the sandwich formula of simulated moments: they count sampling "
              "error and the simulation's noise, not the search's own, which leaves them about "
              "half as wide as they should be (docs/validation.md; --refits counts it)")
             + ", and none of the ways the model differs from the market.",
             "", "## Held-out seeds", "", text.split("\n", 2)[2]]
    data["calibration"] = {"settings": rows, "distance_fit_seeds": score,
                           "distance_held_out": held_distance, "reached": reached,
                           "compared": target.keys,
                           "runs": evaluator.runs, "truth": truth,
                           "refits": refits if refits >= 2 else 0}
    if spec.condition:
        first = evaluator.targets[0]
        scenario = (f"# The fitted settings, in the twin of the recorded market {first.name}.\n"
                    + spec.scenario(fitted, first))
    else:
        scenario = spec.scenario(fitted)
    return "\n".join(lines), data, scenario


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
    parser.add_argument("--refits", type=int, default=0,
                        help="search again this many times from the fit on fresh seeds, and add "
                        "the spread of where it lands to the intervals (2 or more; 0 for none)")
    parser.add_argument("--refit-generations", type=int, default=15)
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--output", type=Path, default=Path("calibrated"),
                        help="directory for the fitted scenario and the report")
    args = parser.parse_args()
    if args.refits < 0 or args.refits == 1:
        parser.error("--refits is 0, for none, or 2 or more: one search alone has no spread")
    if bool(args.real) == bool(args.truth):
        parser.error("give one of --real or --truth")
    spec = read_spec(args.spec)
    for name in ("generations", "seeds", "held_out"):
        if getattr(args, name) is not None:
            setattr(spec, name, getattr(args, name))
    config = validate.Config(bootstrap=args.bootstrap)
    recorded = tapes.read_tape(args.real) if args.real else None
    if spec.condition and recorded is None:
        parser.error("a conditioned spec is fitted to a recording: give --real")
    evaluator = Evaluator(find_tool(args.crowdbook), spec, config,
                          with_openings(recorded) if spec.condition else None)
    truth = None
    if args.truth:
        truth = parse_truth(args.truth, spec)
        seeds = range(TRUTH_SEED, TRUTH_SEED + args.truth_markets)
        real = validate.summarize("truth", evaluator.tape(truth, seeds), config, seed=1)
    else:
        real = validate.summarize(args.real.name, spec.cut(recorded), config, seed=1)
    text, data, scenario = calibrate(spec, evaluator, real, truth=truth,
                                     log=lambda line: print(line, flush=True),
                                     refits=args.refits,
                                     refit_generations=args.refit_generations)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "scenario.toml").write_text(scenario)
    fitted = {row["name"]: float(row["fitted"]) for row in data["calibration"]["settings"]}
    (args.output / "settings.toml").write_text(settings_text(fitted))
    (args.output / "report.md").write_text(text)
    (args.output / "report.json").write_text(json.dumps(data, indent=1, default=float))
    if spec.condition:
        write_twins(spec, fitted, evaluator.targets, args.output / "twins")
    print(text)


def settings_text(values: dict[str, float]) -> str:
    """Settings as TOML, one `name = value` a line, numbers in full."""
    return "".join(f"{name} = {float(value)!r}\n" for name, value in values.items())


def write_twins(spec: Spec, values: dict[str, float], targets: list[tapes.Market],
                out: Path) -> list[Path]:
    """The twin of every recorded market at these settings, one scenario file each, named by its
    market; returns the files in the order of the markets."""
    out.mkdir(parents=True, exist_ok=True)
    paths = []
    for index, market in enumerate(targets):
        name = re.sub(r"[^A-Za-z0-9._-]+", "_", str(market.name)).strip("._") or "market"
        path = out / f"{index:04d}_{name}.toml"
        path.write_text(f"# The fitted settings, in the twin of the recorded market "
                        f"{market.name}.\n" + spec.scenario(values, market))
        paths.append(path)
    return paths


if __name__ == "__main__":
    main()
