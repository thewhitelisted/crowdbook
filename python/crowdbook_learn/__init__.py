"""Many crowdbook markets stepped at once from Python, for learning agents.

A `Pool` holds markets that run on threads of their own, in C++, without holding Python's lock.
Each market has one seat, the learner, which decides at every interval of market time and
whenever one of its own orders fills. Observations, rewards and flags are NumPy arrays that view
the pool's own memory: they are the same arrays after every call, overwritten each time, so copy
what you keep. Calls to one pool from several threads take turns.

    pool = crowdbook_learn.Pool("examples/learning/market.toml", markets=64)
    obs, info = pool.reset(seeds=range(64))
    obs, rewards, terminated, truncated, info = pool.step(actions)  # (64, 2): action, size

Markets are not reset when their episodes end: reset them with a mask, choosing every episode's
seed. `VectorEnv` puts the same pool behind Gymnasium's vector API, when Gymnasium is installed.
docs/design.md in crowdbook's repository describes the observation, the actions and the reward.
"""

from __future__ import annotations

import os
from collections.abc import Iterable, Sequence

import numpy as np

from . import _core
from ._core import ScenarioError

__all__ = ["Pool", "VectorEnv", "ScenarioError", "STATE_NAMES", "EVENT_NAMES"]

STATE_NAMES: list[str] = list(_core.STATE_NAMES)
EVENT_NAMES: list[str] = list(_core.EVENT_NAMES)
EVENT_COLUMNS: int = _core.EVENT_COLUMNS
ORDER_COLUMNS: int = _core.ORDER_COLUMNS

_OBSERVATION = ("state", "orders", "events", "depth", "book", "counts")
_INFO = ("elapsed", "score", "value", "stopped", "seeds", "settings_seeds", "settings")


def _seeds(seeds, count: int, name: str) -> np.ndarray:
    """One unsigned 64-bit seed a market, from a number (seed, seed + 1, ...) or a sequence."""
    if isinstance(seeds, (int, np.integer)):
        if seeds < 0:
            raise ValueError(f"{name} must not be negative")
        return np.arange(int(seeds), int(seeds) + count, dtype=np.uint64)
    values = np.asarray(seeds if isinstance(seeds, np.ndarray) else list(seeds))
    if values.shape != (count,):
        raise ValueError(f"{name} must have one entry a market: {values.shape} for {count} markets")
    if values.dtype == object or not np.issubdtype(values.dtype, np.integer):
        raise TypeError(f"{name} must be whole numbers, not {values.dtype}")
    if np.issubdtype(values.dtype, np.signedinteger) and (values < 0).any():
        raise ValueError(f"{name} must not be negative")
    return np.ascontiguousarray(values, dtype=np.uint64)


class Pool:
    """Markets of one scenario, stepped together. `scenario` is a scenario file's path."""

    def __init__(self, scenario: str | os.PathLike, markets: int, threads: int = 0):
        path = os.fspath(scenario)
        with open(path, encoding="utf-8") as file:
            text = file.read()
        self._init(text, path, markets, threads)

    @classmethod
    def from_text(cls, text: str, markets: int, threads: int = 0, source: str = "scenario"):
        """A pool of the scenario written in `text`."""
        pool = cls.__new__(cls)
        pool._init(text, source, markets, threads)
        return pool

    def _init(self, text: str, source: str, markets: int, threads: int) -> None:
        self._pool = _core.Pool(text, source, markets, threads)
        self._buffers = self._pool.buffers()
        self.observation = {name: self._buffers[name] for name in _OBSERVATION}
        self.info = {name: self._buffers[name] for name in _INFO}

    @property
    def markets(self) -> int:
        return self._pool.markets

    @property
    def threads(self) -> int:
        return self._pool.threads

    @property
    def action_names(self) -> list[str]:
        return self._pool.action_names

    @property
    def sizes(self) -> list[int]:
        return self._pool.sizes

    @property
    def setting_names(self) -> list[str]:
        return self._pool.setting_names

    @property
    def config(self) -> dict:
        return self._pool.config

    @property
    def rewards(self) -> np.ndarray:
        return self._buffers["rewards"]

    @property
    def terminated(self) -> np.ndarray:
        return self._buffers["terminated"]

    @property
    def truncated(self) -> np.ndarray:
        return self._buffers["truncated"]

    def reset(self, seeds, settings_seeds=None, mask=None,
              scenarios: Sequence[str | None] | None = None):
        """Starts a new episode in every market, or in those `mask` selects, and returns the
        observation and the info, new dicts of the pool's arrays. Market i runs with seeds[i] and
        draws its settings with settings_seeds[i], or seeds[i]; a number n stands for n, n + 1,
        and so on. A scenario's text in scenarios[i] replaces the pool's for that episode."""
        count = self.markets
        seed_array = _seeds(seeds, count, "seeds")
        settings = None if settings_seeds is None else _seeds(settings_seeds, count,
                                                              "settings_seeds")
        selected = None
        if mask is not None:
            selected = np.ascontiguousarray(np.asarray(mask, dtype=bool).astype(np.uint8))
            if selected.shape != (count,):
                raise ValueError(f"the mask must have one entry a market, not {selected.shape}")
        if isinstance(scenarios, str):
            raise TypeError("scenarios holds a scenario's text, or None, for each market")
        own = None if scenarios is None else list(scenarios)
        self._pool.reset(seed_array, settings, selected, own)
        return dict(self.observation), dict(self.info)

    def step(self, actions):
        """Takes each market's action, as rows of (action, size) or, with one size, the action
        alone, and returns the observation, the rewards, whether each episode is over, whether
        it was cut short (never: its end is part of the task) and the info."""
        array = np.asarray(actions)
        if not np.issubdtype(array.dtype, np.integer):
            raise TypeError(f"actions must be whole numbers, not {array.dtype}")
        array = array.astype(np.int64, copy=False)
        if array.ndim == 1:
            if len(self.sizes) != 1:
                raise ValueError("with more than one size, each market's action is a pair: "
                                 "the action and the size")
            array = np.stack([array, np.zeros_like(array)], axis=1)
        self._pool.step(np.ascontiguousarray(array))
        return (dict(self.observation), self._buffers["rewards"], self._buffers["terminated"],
                self._buffers["truncated"], dict(self.info))

    def session(self, market: int) -> str:
        """Market `market`'s episode so far as a session file, which `crowdbook replay` and
        `crowdbook report` read."""
        return self._pool.session(market)


try:
    import gymnasium
    from gymnasium import spaces
    from gymnasium.vector.utils import batch_space
except ImportError:  # Gymnasium is optional
    gymnasium = None


def _box(shape: tuple[int, ...]):
    info = np.iinfo(np.int64)
    return spaces.Box(low=info.min, high=info.max, shape=shape, dtype=np.int64)


if gymnasium is not None:

    class VectorEnv(gymnasium.vector.VectorEnv):
        """A pool behind Gymnasium's vector API. Episodes are not reset automatically
        (AutoresetMode.DISABLED): reset the ones that ended with
        reset(seed=..., options={"reset_mask": terminated | truncated}), choosing their seeds.
        Options may also give "settings_seeds" and "scenarios", as Pool.reset takes them. A
        whole-number seed n gives the markets n, n + 1, ... and seeds the environment's own
        generator, which draws the seeds of later resets given no seed, or None for a market."""

        metadata = {"autoreset_mode": gymnasium.vector.AutoresetMode.DISABLED}

        def __init__(self, scenario: str | os.PathLike, num_envs: int, threads: int = 0):
            self.pool = Pool(scenario, num_envs, threads)
            self.num_envs = num_envs
            observation = self.pool.observation
            self.single_observation_space = spaces.Dict({
                name: _box(array.shape[1:]) for name, array in observation.items()})
            self.observation_space = batch_space(self.single_observation_space, num_envs)
            self.single_action_space = spaces.MultiDiscrete(
                [len(self.pool.action_names), len(self.pool.sizes)])
            self.action_space = batch_space(self.single_action_space, num_envs)

        def reset(self, *, seed: int | Iterable[int | None] | None = None,
                  options: dict | None = None):
            options = options or {}
            if isinstance(seed, (int, np.integer)):
                super().reset(seed=int(seed))
            else:
                seed = [int(self.np_random.integers(0, 2**63)) if each is None else each
                        for each in (seed if seed is not None else [None] * self.num_envs)]
            return self._open().reset(seed, options.get("settings_seeds"),
                                      options.get("reset_mask"), options.get("scenarios"))

        def step(self, actions):
            return self._open().step(actions)

        def close_extras(self, **kwargs):
            self.pool = None  # its threads stop once nothing views its arrays

        def _open(self) -> Pool:
            if self.pool is None:
                raise RuntimeError("the environment is closed")
            return self.pool

else:

    class VectorEnv:  # type: ignore[no-redef]
        """Needs Gymnasium: pip install gymnasium."""

        def __init__(self, *args, **kwargs):
            raise ImportError("crowdbook_learn.VectorEnv needs Gymnasium: pip install gymnasium")
