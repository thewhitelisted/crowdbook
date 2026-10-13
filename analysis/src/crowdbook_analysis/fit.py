"""Fits a prediction market's settings to real markets: searches the settings of a fifteen-minute
up-or-down question for the simulated market whose scorecard comes closest to the real one.

    uv run --project analysis crowdbook-polymarket --from 2026-09-28 --to 2026-10-12
    uv run --project analysis crowdbook-fit

The target is the realism scorecard of Polymarket's fifteen-minute Bitcoin questions. The search
goes in two stages, since the true value moves the same way whoever trades: first the news that
moves the value, against how the real value moves (worked out from Bitcoin's price); then the
traders, against everything their trading makes. Each stage changes one setting at a time over a
few candidates, keeping a change that brings the market closer, and goes round until a round
changes nothing. Closeness is the sum of squared differences between the scorecards, each divided
by a tolerance: about what one statistic would be off by between two equally good fits.

The fitted settings go to examples/scenarios/up_or_down.toml; the scorecards of the real markets,
the prediction example and the fit are printed as Markdown.
"""

import argparse
import math
from dataclasses import dataclass, replace
from pathlib import Path

from crowdbook_analysis import polymarket, realism
from crowdbook_analysis.runner import REPO, find_tool

SERIES = "btc-up-or-down-15m"
OUTPUT = REPO / "examples" / "scenarios" / "up_or_down.toml"
SEEDS = 120


@dataclass(frozen=True)
class Settings:
    """What the search may change, starting from a first guess."""

    news_rate: float = 0.03  # news a second
    news_share: float = 0.05  # of the question's uncertainty that news carries
    fast: int = 2  # traders who see the probability within a second or two
    fast_lag: float = 1.0  # seconds, their mean delay
    crowd: int = 20  # traders who see it later, each with a view of its own
    crowd_lag: float = 2.0  # seconds, their mean delay
    crowd_interval: float = 2.0  # seconds between the crowd's looks
    crowd_noise: float = 2.0  # cents of fresh error on each of the crowd's looks
    crowd_threshold: float = 3.0  # cents of edge the crowd waits for
    crowd_error: float = 3.0  # cents of lasting error in what each of the crowd believes
    informed_tail: float = 1.5  # of the sizes of orders from traders who follow the value
    noise_rate: float = 0.01  # market orders a second from each noise trader
    limit_rate: float = 0.4  # limit orders a second from each noise trader
    size_tail: float = 1.3  # of noise traders' order sizes
    maker_intensity: float = 0.35  # the market maker's base half spread is about 1/this cents
    brokers: int = 2  # working large orders in children


TEMPLATE = """\
# A question like "will Bitcoin be up at the end of the next fifteen minutes?": it opens at a coin
# flip with nothing known and resolves fifteen minutes later. Its settings are fitted to
# Polymarket's fifteen-minute Bitcoin questions by `crowdbook-fit` in the analysis package; see
# docs/results.md for how close the fit comes and where it falls short.
#
# Fast traders see the probability about {fast_lag:g} s late and a crowd of slower ones about
# {crowd_lag:g} s late, each with views of their own. Their orders, and the noise traders', are
# mostly small with a few far larger, and brokers work larger orders in pieces.
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
latency = {{ to_exchange = "200us", from_exchange = "200us", jitter = "50us" }}
account = {{ max_position = 1000000000, max_order_quantity = 5 }}
"""


def _duration(seconds: float) -> str:
    """A duration as a scenario file writes it: in whole seconds where it is, else milliseconds."""
    millis = round(seconds * 1000)
    return f"{millis // 1000}s" if millis % 1000 == 0 else f"{millis}ms"


def scenario(settings: Settings) -> str:
    """The scenario file for these settings."""
    brokers = BROKERS.format(brokers=settings.brokers) if settings.brokers > 0 else ""
    return TEMPLATE.format(
        news_rate=settings.news_rate, news_share=settings.news_share, fast=settings.fast,
        crowd=settings.crowd,
        fast_lag=settings.fast_lag, crowd_lag=settings.crowd_lag,
        fast_lag_text=_duration(settings.fast_lag), crowd_lag_text=_duration(settings.crowd_lag),
        crowd_interval_text=_duration(settings.crowd_interval), crowd_error=settings.crowd_error,
        crowd_noise=settings.crowd_noise, crowd_threshold=settings.crowd_threshold,
        informed_tail=settings.informed_tail,
        noise_rate=settings.noise_rate, limit_rate=settings.limit_rate,
        size_tail=settings.size_tail,
        maker_intensity=settings.maker_intensity, brokers_section=brokers)


# Each statistic the fit compares, how to read it from a scorecard, and its tolerance. Rates and
# ratios compare as logarithms, so their tolerance is a fraction.
TRADER_TERMS = [
    ("Brier score", lambda c: c["brier"], 0.01, False),
    ("priced 10 to 20: resolved yes", lambda c: c["low"], 0.03, False),
    ("priced 80 to 90: resolved yes", lambda c: c["high"], 0.03, False),
    ("orders per minute", lambda c: c["orders_per_minute"], 0.15, True),
    ("moves in the first tenth", lambda c: c["moves"]["close"][0], 2.0, False),
    ("moves in the last tenth", lambda c: c["moves"]["close"][-1], 5.0, False),
    ("excess kurtosis of moves", lambda c: c["kurtosis"], 0.5, False),
    ("orders in the first tenth", lambda c: c["activity"][0], 0.02, False),
    ("orders in the last tenth", lambda c: c["activity"][-1], 0.02, False),
    ("largest tenth of sizes against the median", lambda c: c["sizes"]["tail"], 0.2, True),
    ("tail exponent of sizes", lambda c: c["sizes"]["exponent"], 0.15, True),
    ("order signs one apart", lambda c: c["signs"][1], 0.05, False),
    ("order signs ten apart", lambda c: c["signs"][10], 0.04, False),
    ("successive price changes", lambda c: c["bounce"]["correlation"], 0.05, False),
    ("spread implied", lambda c: c["bounce"]["spread"], 0.2, True),
    ("price closest to the value as it was", lambda c: c["lateness"]["delay"], 2.0, False),
]
VALUE_TERMS = [
    ("value's moves in the first tenth", lambda c: c["value"]["first"], 2.0, False),
    ("value's moves in the last tenth", lambda c: c["value"]["last"], 5.0, False),
    ("excess kurtosis of the value's moves", lambda c: c["value"]["kurtosis"], 0.5, False),
]


def distance(card: dict, target: dict, terms: list) -> float:
    total = 0.0
    for _, read, tolerance, logarithmic in terms:
        found, wanted = read(card), read(target)
        if logarithmic:
            gap = math.log(max(found, 1e-9) / max(wanted, 1e-9))
        else:
            gap = found - wanted
        total += (gap / tolerance) ** 2 if math.isfinite(gap) else 1e6
    return total


def evaluate(tool: Path, settings: Settings, seeds: int) -> dict:
    windows = realism.simulated_windows(tool, scenario(settings), range(1, seeds + 1))
    return realism.scorecard(windows)


CANDIDATES = {
    "news_rate": [0.003, 0.01, 0.03, 0.1],
    "news_share": [0.05, 0.15, 0.3, 0.5],
    "fast": [1, 2, 4, 8],
    "fast_lag": [0.2, 0.5, 1.0, 2.0],
    "crowd": [5, 10, 20, 30],
    "crowd_lag": [2.0, 5.0, 10.0, 20.0],
    "crowd_interval": [1.0, 2.0, 5.0, 10.0],
    "crowd_noise": [1.0, 2.0, 3.0],
    "crowd_threshold": [2.0, 3.0, 5.0, 8.0],
    "crowd_error": [1.0, 3.0, 6.0],
    "informed_tail": [0.8, 1.2, 1.6, 2.5],
    "noise_rate": [0.005, 0.01, 0.02, 0.04],
    "limit_rate": [0.1, 0.2, 0.4, 0.8],
    "size_tail": [0.8, 1.0, 1.3, 1.6],
    "maker_intensity": [0.2, 0.35, 0.5, 1.0],
    "brokers": [0, 2, 5, 10],
}


def search(tool: Path, start: Settings, names: list[str], target: dict, terms: list,
           seeds: int) -> tuple[Settings, dict]:
    """Changes one setting at a time over its candidates, keeping whatever brings the scorecard
    closer, until a round through every setting changes nothing."""
    best = start
    card = evaluate(tool, best, seeds)
    score = distance(card, target, terms)
    print(f"start: {score:.1f}", flush=True)
    improved = True
    while improved:
        improved = False
        for name in names:
            for value in CANDIDATES[name]:
                if value == getattr(best, name):
                    continue
                trial = replace(best, **{name: value})
                trial_card = evaluate(tool, trial, seeds)
                trial_score = distance(trial_card, target, terms)
                if trial_score < score:
                    best, card, score = trial, trial_card, trial_score
                    improved = True
                    print(f"{name} = {value}: {score:.1f}", flush=True)
    return best, card


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--seeds", type=int, default=SEEDS)
    parser.add_argument("--data", type=Path, default=polymarket.DATA)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    tool = find_tool(args.crowdbook)

    target = realism.scorecard(realism.real_windows(SERIES, args.data, with_value=True))
    settings, _ = search(tool, Settings(), ["news_rate", "news_share"], target, VALUE_TERMS,
                         args.seeds)
    settings, card = search(tool, settings,
                            [name for name in CANDIDATES if not name.startswith("news")],
                            target, TRADER_TERMS, args.seeds)
    args.output.write_text(scenario(settings))
    print(f"\nfitted: {settings}\n")
    example = realism.scorecard(realism.simulated_windows(
        tool, realism.MARKET.read_text(), range(1, args.seeds + 1)))
    columns = {"real, 15 min": target, "simulated: prediction": example,
               "simulated: up_or_down": card}
    realism.print_scorecard(columns)
    realism.print_calibration(columns)
    print()
    print("| statistic | example: off by | fitted: off by |")
    print("|---|---:|---:|")
    for name, read, tolerance, logarithmic in VALUE_TERMS + TRADER_TERMS:
        cells = []
        for card_ in (example, card):
            gap = (math.log(read(card_) / read(target)) if logarithmic
                   else read(card_) - read(target)) / tolerance
            cells.append(f"{gap:+.1f}")
        print(f"| {name} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
