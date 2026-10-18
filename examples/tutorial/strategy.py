"""The tutorial's strategy (docs/tutorial.md), tested in fitted markets: a maker that joins the
best bid and offer with a few lots, and stops adding to a position past a limit.

    python examples/tutorial/strategy.py assessed/scenario.toml draws
    python examples/tutorial/strategy.py --truth maker_intensity=0.8,noise_market_rate=0.3,follower_lag=10

Each argument is a scenario file, or a directory of them, such as crowdbook-draws' output; with
--truth, the tutorial's market at those settings is tested too. Every row runs the same seeds.
"""

import argparse
from pathlib import Path

import numpy as np
from crowdbook_learn import STATE_NAMES
from crowdbook_learn.evaluate import evaluate

from crowdbook_analysis.calibrate import parse_truth, read_spec

SPEC = Path(__file__).with_name("spec.toml")
SIZE = 2
LIMIT = 10

_POSITION = STATE_NAMES.index("position")
_BID = STATE_NAMES.index("bid_price")
_ASK = STATE_NAMES.index("ask_price")


def join_the_touch(observation, info):
    """Every market's orders at once: a bid at the best bid and an offer at the best offer, each
    of SIZE lots, leaving out the side whose fill would take the position past LIMIT."""
    state = observation["state"]
    position = state[:, _POSITION]
    orders = np.zeros((len(state), 5), dtype=np.int64)
    orders[:, 0] = state[:, _BID]
    orders[:, 1] = np.where(position + SIZE <= LIMIT, SIZE, 0)
    orders[:, 2] = state[:, _ASK]
    orders[:, 3] = np.where(position - SIZE >= -LIMIT, SIZE, 0)
    return orders


def _name(path: Path) -> str:
    if path.is_dir():
        return f"{len(list(path.glob('*.toml')))} scenarios in {path}"
    return str(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scenarios", nargs="*", type=Path,
                        help="scenario files, or directories of them")
    parser.add_argument("--truth", help="also the tutorial's market at these settings, "
                        "name=value,...")
    parser.add_argument("--seeds", type=int, default=200, help="markets each row is judged on")
    args = parser.parse_args()
    rows = [(_name(path), path) for path in args.scenarios]
    if args.truth:
        spec = read_spec(SPEC)
        rows.append(("the truth", spec.scenario(parse_truth(args.truth, spec))))
    if not rows:
        parser.error("give scenarios, --truth, or both")
    # Seeds far from those the fit and the check used.
    seeds = range(9_000_001, 9_000_001 + args.seeds)
    print("| market | made a market (tick-lots) | 95% interval | lots | per lot | markout 10 s |")
    print("|---|---:|---|---:|---:|---:|")
    for name, scenarios in rows:
        s = evaluate(join_the_touch, scenarios, seeds)[1]["summary"]
        print(f"| {name} | {s['made']:+,.1f} | [{s['made_low']:+,.1f}, {s['made_high']:+,.1f}] "
              f"| {s['lots']:,.0f} | {s['made_per_lot']:+.3f} | {s['markouts']['10s']:+.3f} |")


if __name__ == "__main__":
    main()
