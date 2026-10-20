# crowdbook_learn

Many crowdbook markets stepped at once from Python, each through one seat for a learning agent.
The markets run in C++ on threads of their own while Python waits without holding its lock, and
every observation is a NumPy array over the pool's own memory.

```python
import crowdbook_learn

pool = crowdbook_learn.Pool("examples/learning/market.toml", markets=64)
obs, info = pool.reset(seeds=range(64))
obs, rewards, terminated, truncated, info = pool.step(actions)  # 64 rows of (action, size)
pool.reset(seeds=range(64, 128), mask=terminated.copy())        # new episodes where they ended
```

- `Pool(path, markets, threads=0)` builds the markets of a scenario file; `Pool.from_text` takes
  the scenario's text. Threads are every core unless given; results do not depend on them.
- `reset(seeds, settings_seeds=None, mask=None, scenarios=None)` starts an episode in each market
  the mask selects, or in all of them. A number n stands for the seeds n, n + 1, and so on. Each
  episode's settings are drawn from the scenario's ranges with its settings seed, its seed unless
  given; `scenarios[i]`, a scenario's text, replaces the pool's for that episode.
- `step(actions)` takes a row of (action, size) for each market, or just the action when there is
  one size, and runs every market to its next decision. A market whose episode is over must be
  reset before it steps again.
- `observation` and `info` are dicts of read-only arrays, the same arrays after every call: copy
  what you keep. `reset` and `step` return new dicts of those arrays, which callers may change.
  Calls to one pool from several threads take turns. `STATE_NAMES`, `EVENT_NAMES`, `action_names`,
  `sizes` and `setting_names` name their columns and numbers.
- `step_orders(orders)` places explicit orders instead of an action, a row a market: the bid's
  price and size, the offer's price and size, a quantity to take, negative to sell, and
  optionally the take's limit price, at the best price on the other side without one. A side
  with a price or size of 0 has no quote; a quote already at its price keeps its place in the
  queue; a take first cancels the quote on the side it takes from. Any strategy that names its
  prices can trade through it.
- `session(i)` is market i's episode as a session file, which `crowdbook replay` and
  `crowdbook report` read.
- `copy(source, targets)` makes each target market a copy of market `source` as it stands: the
  same episode at the same decision, from which a copy goes on as the source does given the same
  actions, and its own way given others, for strategies that search ahead by trying actions in
  copies. A copy takes the source's decisions again from the start of its episode, so it costs
  what the episode so far cost. It returns the observation and the info, as `reset` does.
- `VectorEnv(path, num_envs, threads=0)` puts a pool behind Gymnasium's vector API, with automatic
  resets disabled: reset the episodes that ended with
  `reset(seed=..., options={"reset_mask": terminated})`. A whole-number seed also seeds the
  environment's generator, which draws the seeds of later resets given none.

`crowdbook_learn.evaluate` runs a strategy in many markets: `evaluate(policy, scenarios, seeds,
scales=(1, 2, 4))` steps a policy that answers each decision with `step_orders`' rows, across one
scenario or several (draws from a fit's uncertainty, from `crowdbook-draws`, or a recording's
twins), and reports what it made a market with a 95% interval, the lots it traded and its share
as a maker, what it made a lot, its quotes' place in the queue (the lots ahead of each when it
reached the book, approximated by the size at its price the seat last saw), the share of the lots
it quoted that filled and how long a filled quote waited, and the size-weighted markouts of its
fills 1, 10 and 60 seconds on: its adverse selection. Each scale multiplies its sizes and runs the same seeds again, so that
what it makes a lot as it grows shows the market's capacity for it. `per_market(decide)` turns a
function of one market's arrays into a policy.

The scenario's `[learning]` section sets the decisions, the actions, the sizes and the settings
drawn for each episode; [docs/scenarios.md](../docs/scenarios.md#learning-optional) lists its keys
and [docs/design.md](../docs/design.md#learning-environment) describes the environment.

## Installing

The module is part of the `crowdbook-engine` package, with the `crowdbook` command:

```bash
pip install .                      # from crowdbook's checkout: CMake 3.25+ and a C++23 compiler
pip install "crowdbook-engine[gymnasium]==0.14.0"
python python/examples/random_policy.py examples/learning/market.toml
```

The second line installs the release's wheel from PyPI, with no compiler, and Gymnasium for
`VectorEnv`, on Linux (x86-64 or arm64, glibc 2.28 or later) or macOS 14 or later on Apple
silicon; on any other platform it builds the engine from its source package, which needs the
compiler.
[docs/quickstart.md](../docs/quickstart.md#install) has the details. NumPy comes with it.

Working on crowdbook itself, CMake builds the module with the rest when it finds Python 3.12 or
later with its development files, into `build/<preset>/python`, which imports as it is:

```bash
cmake --workflow --preset release
PYTHONPATH=build/release/python python3 python/examples/random_policy.py examples/learning/market.toml
```

[requirements.txt](requirements.txt) pins the NumPy and Gymnasium that CI tests with, and `ctest`
runs [tests/test_learn.py](tests/test_learn.py) when NumPy is installed.

## License

The module is part of crowdbook's engine and under its license,
[PolyForm Shield 1.0.0](../LICENSE), unlike the Python client in
[clients/python](../clients/python), which is under MIT. It compiles in nanobind and robin-map,
whose licenses are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
