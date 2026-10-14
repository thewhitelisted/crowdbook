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
  what you keep. `STATE_NAMES`, `EVENT_NAMES`, `action_names`, `sizes` and `setting_names` name
  their columns and numbers.
- `session(i)` is market i's episode as a session file, which `crowdbook replay` and
  `crowdbook report` read.
- `VectorEnv(path, num_envs, threads=0)` puts a pool behind Gymnasium's vector API, with automatic
  resets disabled: reset the episodes that ended with
  `reset(seed=..., options={"reset_mask": terminated})`.

The scenario's `[learning]` section sets the decisions, the actions, the sizes and the settings
drawn for each episode; [docs/scenarios.md](../docs/scenarios.md#learning-optional) lists its keys
and [docs/design.md](../docs/design.md#learning-environment) describes the environment.

## Building

CMake builds the module with the rest of crowdbook when it finds Python 3.11 or later with its
development files, into `build/<preset>/python`:

```bash
cmake --workflow --preset release
PYTHONPATH=build/release/python python3 python/examples/random_policy.py examples/learning/market.toml
```

It needs NumPy, and Gymnasium for `VectorEnv`; [requirements.txt](requirements.txt) pins the
versions CI tests. `ctest` runs [tests/test_learn.py](tests/test_learn.py) when NumPy is installed.

## License

The module is part of crowdbook's engine and under its license,
[PolyForm Shield 1.0.0](../LICENSE), unlike the Python client in
[clients/python](../clients/python), which is under MIT. It compiles in nanobind and robin-map,
whose licenses are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
