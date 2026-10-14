"""Steps many markets at once with actions drawn at random, and reports how fast they ran.

    python3 python/examples/random_policy.py examples/learning/market.toml --markets 64

With the build's module on the path: PYTHONPATH=build/release/python.
"""

import argparse
import time

import numpy as np

import crowdbook_learn


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scenario", help="a scenario file with a [learning] section")
    parser.add_argument("--markets", type=int, default=64)
    parser.add_argument("--threads", type=int, default=0, help="0 for every core")
    parser.add_argument("--steps", type=int, default=2_000)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    pool = crowdbook_learn.Pool(args.scenario, args.markets, args.threads)
    rng = np.random.default_rng(args.seed)
    next_seed = args.seed
    pool.reset(seeds=next_seed)
    next_seed += args.markets
    episodes = 0
    started = time.perf_counter()
    for _ in range(args.steps):
        actions = np.stack([rng.integers(0, len(pool.action_names), args.markets),
                            rng.integers(0, len(pool.sizes), args.markets)], axis=1)
        _, _, terminated, _, _ = pool.step(actions)
        if terminated.any():
            ended = terminated.copy()
            episodes += int(ended.sum())
            pool.reset(seeds=next_seed, mask=ended)
            next_seed += args.markets
    seconds = time.perf_counter() - started
    decisions = args.steps * args.markets
    print(f"{args.markets} markets on {pool.threads} threads: {decisions:,} decisions in "
          f"{seconds:.2f}s, {decisions / seconds:,.0f} a second; {episodes} episodes ended")


if __name__ == "__main__":
    main()
