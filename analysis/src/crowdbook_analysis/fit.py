"""Fits a prediction market's settings to real markets, all settings at once, and says how sure the
fit is: the settings of a fifteen-minute up-or-down question whose scorecard comes closest to
Polymarket's.

    uv run --project analysis crowdbook-polymarket --from 2026-09-28 --to 2026-10-12
    uv run --project analysis crowdbook-fit          # about an hour on four cores

The target is the realism scorecard of Polymarket's fifteen-minute Bitcoin questions, reduced to a
vector of 22 statistics, and the fit is the method of simulated moments (McFadden, 1989):

1. **Tolerances from sampling error.** How much each real statistic could be off by chance comes
   from resampling the real questions (a bootstrap over questions), and how much a simulated one
   could, at the number of seeds the search uses, from resampling simulated questions at the
   starting settings. A statistic's tolerance is the spread of the difference between the two.
2. **A joint search.** Every setting moves at once, by CMA-ES, the covariance matrix adaptation
   evolution strategy (Hansen, 2016), in a box spanning each setting's range, on the same seeds for
   every candidate so that the differences between candidates are not noise. Closeness is the
   sum of squared differences between the scorecards, each over its tolerance.
3. **Judged on other seeds.** The fit and the market it started from are scored on 400 seeds the
   search never used.
4. **Uncertainties.** The settings' covariance is that of a simulated-moments estimate: the
   statistics' derivatives with respect to the settings at the fit, from central differences on
   common seeds, around the covariance of the real and simulated statistics. It counts sampling
   error, not the ways the simulated market differs from the real one.
5. **Ranges of settings.** The scorecard is measured at settings drawn around the fit, a third
   either way, to show how far each statistic moves when the settings do.

The fitted settings go to examples/scenarios/up_or_down.toml, which charges Polymarket's taker fee;
the tables, as Markdown, go to standard output.
"""

import argparse
import math
from dataclasses import asdict, dataclass, replace
from pathlib import Path

import numpy as np

from crowdbook_analysis import polymarket, realism
from crowdbook_analysis.runner import REPO, find_tool

SERIES = "btc-up-or-down-15m"
OUTPUT = REPO / "examples" / "scenarios" / "up_or_down.toml"
FIT_SEEDS = range(1, 121)  # every candidate is judged on these
HELD_OUT = range(realism.FIRST_SEED, realism.FIRST_SEED + 400)  # never seen by the search
JACOBIAN_SEEDS = range(200_001, 200_401)
RANGE_SEEDS = range(300_001, 300_081)
BOOTSTRAP = 100
GENERATIONS = 80
RANGE_DRAWS = 40
TAKER_FEE = 1.75  # cents a share at 50: Polymarket's 0.07 p (1 - p) dollars, measured
MARKET_PROTECTION = 10


@dataclass(frozen=True)
class Settings:
    """What the fit may change. The defaults are the market M18 fitted one setting at a time."""

    news_rate: float = 0.01  # news a second
    news_share: float = 0.05  # of the question's uncertainty that news carries
    fast: int = 4  # traders who see the probability within a second or two
    fast_lag: float = 1.0  # seconds, their mean delay
    crowd: int = 10  # traders who see it later, each with a view of its own
    crowd_lag: float = 10.0  # seconds, their mean delay
    crowd_interval: float = 10.0  # seconds between the crowd's looks
    crowd_noise: float = 1.0  # cents of fresh error on each of the crowd's looks
    crowd_threshold: float = 5.0  # cents of edge the crowd waits for
    crowd_error: float = 1.0  # cents of lasting error in what each of the crowd believes
    informed_tail: float = 0.8  # of the sizes of orders from traders who follow the value
    noise_rate: float = 0.005  # market orders a second from each noise trader
    limit_rate: float = 0.2  # limit orders a second from each noise trader
    size_tail: float = 1.0  # of noise traders' order sizes
    maker_intensity: float = 0.2  # the market maker's base half spread is about 1/this cents
    brokers: int = 2  # working large orders in children
    broker_interval_spread: float = 0.0  # how far brokers' waits between children stray
    broker_size_spread: float = 0.0  # how far their children's sizes stray
    taker_fee: float = TAKER_FEE  # not fitted: measured from the chain
    # Not fitted: how far past the last trade a noise trader's market order may go, in cents. It
    # leaves the scorecard as it was, since simulated orders already move the price past 10 cents
    # more rarely than real ones, and it keeps a maker quoting far from the price from making
    # money on market orders that would run to its quotes in a one-sided book.
    market_protection: int = MARKET_PROTECTION


@dataclass(frozen=True)
class Range:
    """Where the search may take a setting, and how it moves: in proportion (on a log scale) or
    by steps, and in whole numbers or not."""

    low: float
    high: float
    log: bool = True
    whole: bool = False


# The ranges five settings first ran into (fast_lag, crowd, crowd_noise, informed_tail and
# maker_intensity) are wider than when the fitted market was found, so that the next fit can go
# further if it needs to.
RANGES = {
    "news_rate": Range(0.002, 0.1),
    "news_share": Range(0.01, 0.5),
    "fast": Range(1, 12, whole=True),
    "fast_lag": Range(0.05, 5.0),
    "crowd": Range(2, 80, whole=True),
    "crowd_lag": Range(1.0, 60.0),
    "crowd_interval": Range(0.5, 30.0),
    "crowd_noise": Range(0.2, 12.0),
    "crowd_threshold": Range(0.5, 12.0),
    "crowd_error": Range(0.2, 10.0),
    "informed_tail": Range(0.4, 3.0),
    "noise_rate": Range(0.001, 0.05),
    "limit_rate": Range(0.05, 1.5),
    "size_tail": Range(0.6, 3.0),
    "maker_intensity": Range(0.05, 2.0),
    "brokers": Range(0, 8, log=False, whole=True),
    "broker_interval_spread": Range(0.0, 0.9, log=False),
    "broker_size_spread": Range(0.0, 0.9, log=False),
}
FITTED = list(RANGES)


def to_unit(settings: Settings) -> np.ndarray:
    """The settings as a point of the unit box, 0 at each setting's low end and 1 at its high."""
    point = []
    for name in FITTED:
        span, value = RANGES[name], float(getattr(settings, name))
        if span.log:
            point.append(math.log(value / span.low) / math.log(span.high / span.low))
        else:
            point.append((value - span.low) / (span.high - span.low))
    return np.array(point)


def from_unit(point: np.ndarray, base: Settings = Settings()) -> Settings:
    """The settings at a point of the unit box, which is clipped to the box first."""
    changes = {}
    for name, u in zip(FITTED, np.clip(point, 0.0, 1.0), strict=True):
        span = RANGES[name]
        value = (span.low * (span.high / span.low) ** u if span.log
                 else span.low + (span.high - span.low) * u)
        changes[name] = int(round(value)) if span.whole else float(value)
    return replace(base, **changes)


TEMPLATE = """\
# A question like "will Bitcoin be up at the end of the next fifteen minutes?": it opens at a coin
# flip with nothing known and resolves fifteen minutes later. Its settings are fitted to
# Polymarket's fifteen-minute Bitcoin questions by `crowdbook-fit` in the analysis package, all at
# once; see docs/results.md for how close the fit comes, how sure it is and where it falls short.
#
# Fast traders see the probability about {fast_lag:.2g} s late and a crowd of slower ones about
# {crowd_lag:.2g} s late, each with views of their own. Their orders, and the noise traders', are
# mostly small with a few far larger{brokers_clause}. Takers pay Polymarket's fee: 0.07 p (1 - p)
# dollars a share at a price of p dollars, and noise traders' market orders go at most
# {market_protection} cents past the last trade.
#
#   crowdbook run examples/scenarios/up_or_down.toml
#   crowdbook play examples/scenarios/up_or_down.toml

seed = 1
duration = "900s"

[prediction]
probability = 0.5
news_rate = {news_rate:.4f}
news_share = {news_share:.3f}
step = "1s"

[exchange]
depth_levels = 10
taker_fee = {taker_fee:.2f}
fee_curve = "probability"

[participant]
latency = {{ to_exchange = "1ms", from_exchange = "1ms" }}
account = {{ max_position = 500, max_order_quantity = 50 }}

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 40
limit_rate = {limit_rate:.3f}
market_rate = {noise_rate:.4f}
cancel_rate = 0.1
max_offset = 3
min_size = 1
max_size = 2000
size_tail = {size_tail:.3f}     # P(size > q) = (1 / q)^{size_tail:.3f}
market_protection = {market_protection}  # cents past the last trade a market order may go
latency = {{ to_exchange = "300us", from_exchange = "300us", jitter = "100us" }}
account = {{ max_position = 1000000, max_order_quantity = 2000 }}

[[agents]]
type = "market_maker"
name = "maker"
volatility = 1.0
intensity = {maker_intensity:.3f}
quote_size = 10
max_inventory = 200
requote_interval = "250ms"
post_only = true
latency = {{ to_exchange = "20us", from_exchange = "20us" }}
account = {{ max_position = 220, max_order_quantity = 10 }}

[[agents]]
type = "informed"
name = "fast"
count = {fast}
interval = "500ms"
noise = 1.0
error = 1.0
error_memory = "300s"
lag = "{fast_lag_text}"
threshold = 2.0
order_size = 5
size_tail = {informed_tail:.3f}
max_position = 600
latency = {{ to_exchange = "200us", from_exchange = "200us", jitter = "50us" }}
account = {{ max_position = 600, max_order_quantity = 600 }}

[[agents]]
type = "informed"
name = "crowd"           # slower, with views of their own
count = {crowd}
interval = "{crowd_interval_text}"
noise = {crowd_noise:.3f}
error = {crowd_error:.3f}
error_memory = "300s"
lag = "{crowd_lag_text}"
threshold = {crowd_threshold:.3f}
order_size = 1
size_tail = {informed_tail:.3f}
max_position = 300
latency = {{ to_exchange = "200us", from_exchange = "200us", jitter = "50us" }}
account = {{ max_position = 300, max_order_quantity = 300 }}
{brokers_section}"""

BROKERS = """
[[agents]]
type = "execution"
name = "brokers"
count = {brokers}
style = "twap"
min_parent = 20
parent_tail = 1.5
max_parent = 2000
pause = "60s"
interval = "1s"
child_size = 5
interval_spread = {interval_spread:.3f}
size_spread = {size_spread:.3f}
latency = {{ to_exchange = "200us", from_exchange = "200us", jitter = "50us" }}
account = {{ max_position = 1000000000, max_order_quantity = 10 }}
"""


def _duration(seconds: float) -> str:
    """A duration as a scenario file writes it: in whole seconds where it is, else milliseconds."""
    millis = round(seconds * 1000)
    return f"{millis // 1000}s" if millis % 1000 == 0 else f"{millis}ms"


def scenario(settings: Settings) -> str:
    """The scenario file for these settings."""
    brokers = (BROKERS.format(brokers=settings.brokers,
                              interval_spread=settings.broker_interval_spread,
                              size_spread=settings.broker_size_spread)
               if settings.brokers > 0 else "")
    values = asdict(settings)
    clause = ", and brokers work larger orders in pieces" if settings.brokers > 0 else ""
    return TEMPLATE.format(
        **values, brokers_clause=clause, fast_lag_text=_duration(settings.fast_lag),
        crowd_lag_text=_duration(settings.crowd_lag),
        crowd_interval_text=_duration(settings.crowd_interval), brokers_section=brokers)


# The statistics the fit compares: a name, how to read it from a scorecard, and whether it is a
# rate or a ratio, compared as a logarithm.
STATISTICS = [
    ("value's moves in the first tenth", lambda c: c["value"]["first"], False),
    ("value's moves in the last tenth", lambda c: c["value"]["last"], False),
    ("excess kurtosis of the value's moves", lambda c: c["value"]["kurtosis"], False),
    ("Brier score", lambda c: c["brier"], False),
    ("priced 10 to 20: resolved yes", lambda c: c["low"], False),
    ("priced 80 to 90: resolved yes", lambda c: c["high"], False),
    ("moves in the first tenth", lambda c: c["moves"]["close"][0], False),
    ("moves in the fifth tenth", lambda c: c["moves"]["close"][4], False),
    ("moves in the last tenth", lambda c: c["moves"]["close"][-1], False),
    ("excess kurtosis of moves", lambda c: c["kurtosis"], False),
    ("last price below 10 or above 90", lambda c: c["closing"], False),
    ("orders per minute", lambda c: c["orders_per_minute"], True),
    ("orders in the first tenth", lambda c: c["activity"][0], False),
    ("orders in the fifth tenth", lambda c: c["activity"][4], False),
    ("orders in the last tenth", lambda c: c["activity"][-1], False),
    ("largest tenth of sizes against the median", lambda c: c["sizes"]["tail"], True),
    ("tail exponent of sizes", lambda c: c["sizes"]["exponent"], True),
    ("order signs one apart", lambda c: c["signs"][1], False),
    ("order signs ten apart", lambda c: c["signs"][10], False),
    ("successive price changes", lambda c: c["bounce"]["correlation"], False),
    ("spread implied", lambda c: c["bounce"]["spread"], True),
    ("price closest to the value as it was", lambda c: c["lateness"]["delay"], False),
]
# Left out: how far the price is from the value. A real question's value is a stand-in worked out
# from Bitcoin's price, and the distance is partly the stand-in's own error, which a fitted market
# should not learn to copy.


def statistics(card: dict) -> np.ndarray:
    """A scorecard as the vector the fit compares, with rates and ratios as logarithms."""
    values = []
    for _, read, logarithmic in STATISTICS:
        value = float(read(card))
        values.append(math.log(value) if logarithmic and value > 0 else
                      (-math.inf if logarithmic else value))
    return np.array(values)


def distance(found: np.ndarray, target: np.ndarray, tolerance: np.ndarray) -> float:
    """The sum of squared differences, each over its tolerance; a statistic that could not be
    measured counts as far off."""
    gaps = (found - target) / tolerance
    return float(np.sum(np.where(np.isfinite(gaps), gaps**2, 1e6)))


def bootstrap(windows: list, draws: int, seed: int) -> np.ndarray:
    """The statistics of `draws` resamples of the windows, with replacement: one row each."""
    rng = np.random.default_rng(seed)
    rows = []
    for _ in range(draws):
        sample = [windows[i] for i in rng.integers(0, len(windows), len(windows))]
        rows.append(statistics(realism.scorecard(sample)))
    found = np.array(rows)
    return found[np.all(np.isfinite(found), axis=1)]


class Evaluator:
    """Scores settings by simulating them, counting every market run."""

    def __init__(self, tool: Path):
        self.tool = tool
        self.runs = 0

    def windows(self, settings: Settings, seeds) -> list:
        self.runs += len(seeds)
        return realism.simulated_windows(self.tool, scenario(settings), seeds)

    def statistics(self, settings: Settings, seeds) -> np.ndarray:
        return statistics(realism.scorecard(self.windows(settings, seeds)))


class Cma:
    """CMA-ES, minimizing a function of a point (Hansen, 2016, with its default settings): each
    generation samples candidates around a mean from a normal distribution, then moves the mean
    toward the best of them and adapts the distribution's shape and size to the steps that
    worked."""

    def __init__(self, mean: np.ndarray, sigma: float, seed: int):
        n = len(mean)
        self.n, self.mean, self.sigma = n, np.array(mean, dtype=float), sigma
        self.size = 4 + int(3 * math.log(n))
        self.parents = self.size // 2
        weights = math.log(self.parents + 0.5) - np.log(np.arange(1, self.parents + 1))
        self.weights = weights / weights.sum()
        self.mueff = 1.0 / float(np.sum(self.weights**2))
        self.cc = (4 + self.mueff / n) / (n + 4 + 2 * self.mueff / n)
        self.cs = (self.mueff + 2) / (n + self.mueff + 5)
        self.c1 = 2 / ((n + 1.3) ** 2 + self.mueff)
        self.cmu = min(1 - self.c1,
                       2 * (self.mueff - 2 + 1 / self.mueff) / ((n + 2) ** 2 + self.mueff))
        self.damps = 1 + 2 * max(0.0, math.sqrt((self.mueff - 1) / (n + 1)) - 1) + self.cs
        self.chi = math.sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n))
        self.pc, self.ps, self.covariance = np.zeros(n), np.zeros(n), np.eye(n)
        self.rng = np.random.default_rng(seed)
        self.generation = 0

    def ask(self) -> np.ndarray:
        values, vectors = np.linalg.eigh(self.covariance)
        self._vectors, self._scales = vectors, np.sqrt(np.maximum(values, 1e-20))
        steps = self.rng.standard_normal((self.size, self.n)) * self._scales @ vectors.T
        return self.mean + self.sigma * steps

    def tell(self, points: np.ndarray, scores: np.ndarray) -> None:
        best = points[np.argsort(scores)[: self.parents]]
        steps = (best - self.mean) / self.sigma
        step = self.weights @ steps
        self.mean = self.mean + self.sigma * step
        whitening = self._vectors @ np.diag(1 / self._scales) @ self._vectors.T
        self.ps = ((1 - self.cs) * self.ps
                   + math.sqrt(self.cs * (2 - self.cs) * self.mueff) * whitening @ step)
        self.generation += 1
        norm = float(np.linalg.norm(self.ps))
        held = (norm / math.sqrt(1 - (1 - self.cs) ** (2 * self.generation)) / self.chi
                < 1.4 + 2 / (self.n + 1))
        self.pc = (1 - self.cc) * self.pc + held * math.sqrt(
            self.cc * (2 - self.cc) * self.mueff) * step
        rank = (steps.T * self.weights) @ steps
        self.covariance = ((1 - self.c1 - self.cmu) * self.covariance
                           + self.c1 * (np.outer(self.pc, self.pc)
                                        + (not held) * self.cc * (2 - self.cc) * self.covariance)
                           + self.cmu * rank)
        self.sigma *= math.exp((self.cs / self.damps) * (norm / self.chi - 1))


def search(evaluator: Evaluator, start: Settings, target: np.ndarray, tolerance: np.ndarray,
           generations: int = GENERATIONS) -> tuple[Settings, float, list[tuple]]:
    """CMA-ES over every setting at once, in the unit box, on the fit's seeds. Returns the best
    settings seen, their distance, and every candidate as (point, distance)."""
    cma = Cma(to_unit(start), sigma=0.12, seed=1)
    best = start
    best_score = distance(evaluator.statistics(start, FIT_SEEDS), target, tolerance)
    history = [(to_unit(start), best_score)]
    print(f"start: {best_score:.1f}", flush=True)
    for generation in range(generations):
        points = cma.ask()
        scores = []
        for point in points:
            # A point outside the box is scored at the nearest point inside, which is what the
            # settings are; the search is told it scored worse the further out it was, so that
            # it comes back in.
            settings = from_unit(point, start)
            outside = float(np.sum((point - np.clip(point, 0.0, 1.0)) ** 2))
            score = distance(evaluator.statistics(settings, FIT_SEEDS), target, tolerance)
            history.append((np.clip(point, 0.0, 1.0), score))
            scores.append(score + 1e4 * outside)
            if score < best_score:
                best, best_score = settings, score
        cma.tell(points, np.array(scores))
        print(f"generation {generation + 1}: best {min(scores):.1f}, overall {best_score:.1f}, "
              f"step {cma.sigma:.3f}", flush=True)
    return best, best_score, history


def derivatives(evaluator: Evaluator, settings: Settings) -> np.ndarray:
    """How each statistic moves with each setting at `settings`, in the unit box, by differences
    on common seeds: a step of 0.05 either way, or for a whole number the neighbouring whole
    numbers, kept within the range."""
    center = to_unit(settings)
    columns = []
    for index, name in enumerate(FITTED):
        span = RANGES[name]
        high, low = center.copy(), center.copy()
        if span.whole:
            value = getattr(settings, name)
            high = to_unit(replace(settings, **{name: min(value + 1, int(span.high))}))
            low = to_unit(replace(settings, **{name: max(value - 1, int(span.low))}))
        else:
            high[index] = min(center[index] + 0.05, 1.0)
            low[index] = max(center[index] - 0.05, 0.0)
        above = evaluator.statistics(from_unit(high, settings), JACOBIAN_SEEDS)
        below = evaluator.statistics(from_unit(low, settings), JACOBIAN_SEEDS)
        columns.append((above - below) / (high[index] - low[index]))
        print(f"derivative {name}: done", flush=True)
    return np.column_stack(columns)


def uncertainty(jacobian: np.ndarray, covariance: np.ndarray, tolerance: np.ndarray) -> np.ndarray:
    """The covariance of the fitted point in the unit box, for a fit that minimized
    sum((m(θ) − m̂)² / tolerance²) when m(θ) − m̂ has covariance `covariance` (the sandwich
    formula of simulated moments)."""
    weight = np.diag(1 / tolerance**2)
    bread = np.linalg.pinv(jacobian.T @ weight @ jacobian)
    meat = jacobian.T @ weight @ covariance @ weight @ jacobian
    return bread @ meat @ bread


def settings_table(fitted: Settings, start: Settings, covariance: np.ndarray,
                   jacobian: np.ndarray) -> None:
    """Each setting as M18 left it and as fitted, with a 68% interval from its standard error in
    the unit box, mapped back to the setting's own scale. Two kinds of setting get no interval: one
    no statistic moves with at the fit, which the statistics cannot pin down at all (the formula
    would give it none, not an infinite one), and one at the edge of its range, where the formula
    does not hold. A setting whose standard error spans half the box or more is not pinned down
    either, and an interval that runs past the range's edge is marked as cut short. Whole numbers
    get intervals as if they could take any value between."""
    print("| setting | M18 | fitted | 68% interval |")
    print("|---|---:|---:|---:|")
    center = to_unit(fitted)
    errors = np.sqrt(np.maximum(np.diag(covariance), 0.0))
    reach = np.linalg.norm(jacobian, axis=0)
    for index, name in enumerate(FITTED):
        span = RANGES[name]

        def value(u: float, s: Range = span) -> float:
            u = min(max(u, 0.0), 1.0)
            return s.low * (s.high / s.low) ** u if s.log else s.low + (s.high - s.low) * u

        if reach[index] <= 1e-9 * reach.max():
            bounds = "not pinned down: no statistic moves with it here"
        elif center[index] <= 0.01 or center[index] >= 0.99:
            bounds = "at the edge of its range"
        elif errors[index] >= 0.5:
            bounds = "not pinned down"
        else:
            bounds = (f"{value(center[index] - errors[index]):.3g} to "
                      f"{value(center[index] + errors[index]):.3g}")
            if center[index] - errors[index] < 0.0 or center[index] + errors[index] > 1.0:
                # The search could go no further, so the interval is cut short and the formula,
                # which assumes the best point lies inside the range, is only a guide.
                bounds += ", cut short by the range's edge"
        print(f"| {name} | {getattr(start, name):.3g} | {getattr(fitted, name):.3g} | "
              f"{bounds} |")
    print()


def comparison_table(cards: dict[str, np.ndarray], target: np.ndarray,
                     tolerance: np.ndarray) -> None:
    """How far each statistic is from the real one, in tolerances, for each simulated market."""
    print("| statistic | tolerance | " + " | ".join(f"{name}: off by" for name in cards) + " |")
    print("|---|---:|" + "---:|" * len(cards))
    for index, (name, _, logarithmic) in enumerate(STATISTICS):
        cells = [f"{(found[index] - target[index]) / tolerance[index]:+.1f}"
                 for found in cards.values()]
        shown = f"{math.expm1(tolerance[index]):.0%}" if logarithmic else f"{tolerance[index]:.3g}"
        print(f"| {name} | {shown} | " + " | ".join(cells) + " |")
    total = [f"{distance(found, target, tolerance):.0f}" for found in cards.values()]
    print("| **distance** | | " + " | ".join(total) + " |")
    print()


def ranges_table(evaluator: Evaluator, fitted: Settings, target: np.ndarray,
                 tolerance: np.ndarray) -> None:
    """The scorecard at settings drawn around the fit: each setting a third either way, drawn
    uniformly on its scale, whole numbers rounded. For each statistic, the real value, the fit's,
    the 5th to 95th percentile across the draws, and how often a draw lands within two tolerances
    of the real value."""
    rng = np.random.default_rng(7)
    rows = []
    for _ in range(RANGE_DRAWS):
        changes = {}
        for name in FITTED:
            span = RANGES[name]
            drawn = getattr(fitted, name) * rng.uniform(2 / 3, 4 / 3)
            drawn = min(max(drawn, span.low), span.high)
            changes[name] = int(round(drawn)) if span.whole else float(drawn)
        rows.append(evaluator.statistics(replace(fitted, **changes), RANGE_SEEDS))
    found = np.array(rows)
    at_fit = evaluator.statistics(fitted, RANGE_SEEDS)
    print("| statistic | real | fitted | 5th to 95th percentile over the draws | "
          "within two tolerances |")
    print("|---|---:|---:|---:|---:|")
    for index, (name, _, logarithmic) in enumerate(STATISTICS):
        column = found[:, index]
        show = (lambda x: f"{math.exp(x):.3g}") if logarithmic else (lambda x: f"{x:.3g}")
        low, high = np.nanpercentile(column, [5, 95])
        within = np.mean(np.abs(column - target[index]) <= 2 * tolerance[index])
        print(f"| {name} | {show(target[index])} | {show(at_fit[index])} | "
              f"{show(low)} to {show(high)} | {within:.0%} |")
    print()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--data", type=Path, default=polymarket.DATA)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--generations", type=int, default=GENERATIONS)
    args = parser.parse_args()
    evaluator = Evaluator(find_tool(args.crowdbook))
    start = Settings()

    real = realism.real_windows(SERIES, args.data, with_value=True)
    target = statistics(realism.scorecard(real))
    real_spread = np.cov(bootstrap(real, BOOTSTRAP, seed=1), rowvar=False)
    simulated = evaluator.windows(start, FIT_SEEDS)
    simulated_spread = np.cov(bootstrap(simulated, BOOTSTRAP, seed=2), rowvar=False)
    tolerance = np.sqrt(np.diag(real_spread) + np.diag(simulated_spread))
    print(f"{len(real)} real questions; tolerances from {BOOTSTRAP} resamples\n", flush=True)

    fitted, score, _ = search(evaluator, start, target, tolerance, args.generations)
    args.output.write_text(scenario(fitted))
    print(f"\nfitted, distance {score:.1f} on the fit's seeds: {fitted}\n", flush=True)

    jacobian = derivatives(evaluator, fitted)
    # Sampling error in the real statistics, and simulation noise at the fit's number of seeds.
    covariance = uncertainty(jacobian, real_spread + simulated_spread, tolerance)
    print(f"\nmarket runs so far: {evaluator.runs}\n")

    example = Settings()
    cards = {
        "M18": evaluator.statistics(example, HELD_OUT),
        "fitted": evaluator.statistics(fitted, HELD_OUT),
    }
    print(f"### Held-out seeds ({len(HELD_OUT)} markets each)\n")
    comparison_table(cards, target, tolerance)
    print("### Settings\n")
    settings_table(fitted, example, covariance, jacobian)
    print("### Ranges of settings\n")
    ranges_table(evaluator, fitted, target, tolerance)
    print(f"market runs in all: {evaluator.runs}")


if __name__ == "__main__":
    main()
