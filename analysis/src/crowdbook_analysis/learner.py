"""A learning probe for the no-false-edge check: a small strategy trained in the market itself, so
that an edge no fixed rule takes, but a learner would, is found too.

    uv run --project analysis crowdbook-sweep my_fitted_market.toml   # with the generic probes

The generic probes each follow one rule. A market fitted to a real one can pay what none of them
does, such as how slowly its price follows the value, read from several signals at once: a
learner trained there finds it, and a strategy trained in the fitted market would trade it,
though the real market need not pay it. This probe is that learner, small enough to train in
minutes: each decision it reads eleven numbers any seat could compute from what it heard, the
flow of aggressive orders and the mid's path over three horizons, the touch's imbalance, the
spread, its own position and the last trade, and maps them, through three linear scores, to a
position it takes toward at the touch and to a bid and an offer it rests at the best prices or
not. Its weights are searched by CMA-ES on what it makes, after fees, valued at the true value
up to its last decision (a settlement's jump left out, which only adds noise), on the search's
markets, a fresh set each generation and the same for every candidate
of one; the final mean is judged on fresh markets, like each generic family's best, and flagged
when its interval there lies above zero. It trades through `crowdbook_learn`'s pool as the
learning seat, from the scenario's `[participant]`, which the probe replaces with its own latency
and limits, and never sees the true value.
"""

from __future__ import annotations

import math
import os
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from crowdbook_analysis.cma import Cma

SEARCH_SEED = 700_001
CONFIRM_SEED = 800_001

HALF_LIVES = (2.0, 20.0, 120.0)  # seconds
FEATURES = (*(f"flow {h:g}s" for h in HALF_LIVES), *(f"trend {h:g}s" for h in HALF_LIVES),
            "touch imbalance", "spread", "position", "last trade", "bias")
HEADS = ("target", "bid", "ask")

_TRADE, _FILL = 1, 4
_HEADER = re.compile(r"^\s*\[\[?\s*([A-Za-z_\"'][\w.\-\"' ]*?)\s*\]\]?\s*(#.*)?$")


def learn_module():
    """crowdbook_learn: installed, or from the build beside the engine found as the sweep finds
    it ($CROWDBOOK, else build/release)."""
    try:
        import crowdbook_learn
        return crowdbook_learn
    except ImportError:
        pass
    from crowdbook_analysis.runner import DEFAULT_TOOL
    tool = Path(os.environ.get("CROWDBOOK") or DEFAULT_TOOL)
    for build in (tool.parent.parent, DEFAULT_TOOL.parent.parent):
        if (build / "python" / "crowdbook_learn").is_dir():
            sys.path.insert(0, str(build / "python"))
            try:
                import crowdbook_learn
                return crowdbook_learn
            except ImportError:
                sys.path.pop(0)
    raise SystemExit("the learning probe needs crowdbook_learn: build it with "
                     "-DCROWDBOOK_BUILD_PYTHON=ON, put build/release/python on PYTHONPATH, or "
                     "pass --no-learner")


def without_tables(text: str, names: tuple[str, ...]) -> str:
    """The scenario's text with the tables called `names`, and their subtables, left out."""
    kept, skipping = [], False
    for line in text.splitlines():
        header = _HEADER.match(line)
        if header:
            name = header.group(1).replace(" ", "").replace('"', "").replace("'", "")
            skipping = any(name == n or name.startswith(n + ".") for n in names)
        if not skipping:
            kept.append(line)
    return "\n".join(kept) + "\n"


def seat(text: str, *, latency: str, size: int, max_position: int, interval: str) -> str:
    """The scenario with the probe's seat: its own [participant] and [learning] in place of any
    the scenario has, with no feed, so that it learns from the market alone."""
    out = without_tables(text, ("participant", "learning"))
    # Valued as the sweep values its probes: at the true value where there is one, at the last
    # trade otherwise, never closed through the book.
    parsed = tomllib.loads(out)
    mark = "value" if "fundamental" in parsed or "prediction" in parsed else "last"
    out += (f'\n[participant]\nlatency = {{ to_exchange = "{latency}", '
            f'from_exchange = "{latency}" }}\n'
            f"account = {{ max_position = {max_position}, max_order_quantity = {size} }}\n"
            f'\n[learning]\ninterval = "{interval}"\nwarmup = "{interval}"\n'
            f"sizes = [{size}]\ndepth_levels = 1\nmax_depth_updates = 0\nmax_events = 512\n"
            f'reward_mark = "{mark}"\nclose = "{mark}"\n')
    tomllib.loads(out)  # a clear error here rather than in the pool
    return out


def _heard(observation: dict) -> tuple[np.ndarray, np.ndarray]:
    """The event rows heard since the last decision, as far as the most any market kept, and
    which of them each market kept."""
    events = observation["events"]
    kept = np.minimum(observation["counts"][:, 0], events.shape[1])
    width = int(kept.max()) if len(kept) else 0
    return events[:, :width], np.arange(width)[None, :] < kept[:, None]


def _nanoseconds(duration: str) -> int:
    """A duration such as "1s", "250ms" or "2m" in nanoseconds."""
    match = re.fullmatch(r"\s*([0-9.]+)\s*(ns|us|ms|s|m|h)\s*", duration)
    if not match:
        raise ValueError(f"not a duration: {duration!r}")
    scale = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9, "m": 60e9, "h": 3600e9}[match.group(2)]
    return round(float(match.group(1)) * scale)


class Features:
    """The probe's view, kept for every market of a pool and updated from each observation: what
    any seat hearing the market could compute, with nothing of the true value."""

    def __init__(self, markets: int, names: dict):
        self.n = markets
        self._time = names["state"].index("time")
        self._position = names["state"].index("position")
        self._bid = names["state"].index("bid_price")
        self._bid_size = names["state"].index("bid_quantity")
        self._ask = names["state"].index("ask_price")
        self._ask_size = names["state"].index("ask_quantity")
        self._last = names["state"].index("last_trade")
        self._limit = names["state"].index("max_position")
        self.decay_rates = np.log(2) / np.array(HALF_LIVES)
        self.reset(np.ones(markets, dtype=bool))

    def reset(self, mask: np.ndarray) -> None:
        if not hasattr(self, "signed"):
            self.signed = np.zeros((self.n, len(HALF_LIVES)))
            self.volume = np.zeros((self.n, len(HALF_LIVES)))
            self.average = np.zeros((self.n, len(HALF_LIVES)))
            self.mid = np.zeros(self.n)
            self.seen = np.zeros(self.n)  # the time of the last decision, in nanoseconds
        self.signed[mask] = 0.0
        self.volume[mask] = 0.0
        self.average[mask] = np.nan
        self.mid[mask] = np.nan
        self.seen[mask] = np.nan

    def update(self, observation: dict) -> np.ndarray:
        """The features of this decision, a row a market, from its observation."""
        state = observation["state"]
        events, rows = _heard(observation)
        trades = rows & (events[:, :, 0] == _TRADE)
        quantity = np.where(trades, events[:, :, 3], 0).astype(float)
        signed = (quantity * events[:, :, 4]).sum(axis=1)
        volume = quantity.sum(axis=1)

        time = state[:, self._time].astype(float)
        elapsed = np.where(np.isnan(self.seen), 0.0, (time - self.seen) / 1e9)
        self.seen = time
        decay = np.exp(-elapsed[:, None] * self.decay_rates[None, :])
        self.signed = self.signed * decay + signed[:, None]
        self.volume = self.volume * decay + volume[:, None]

        bid, ask = state[:, self._bid].astype(float), state[:, self._ask].astype(float)
        both = (bid > 0) & (ask > 0)
        self.mid = np.where(both, (bid + ask) / 2, self.mid)
        fresh = np.isnan(self.average)
        self.average = np.where(fresh, self.mid[:, None],
                                self.average * decay + self.mid[:, None] * (1 - decay))

        x = np.zeros((self.n, len(FEATURES)))
        k = len(HALF_LIVES)
        x[:, :k] = self.signed / np.maximum(self.volume, 1.0)
        x[:, k:2 * k] = np.tanh(np.nan_to_num(self.mid[:, None] - self.average) / 2)
        sizes = state[:, self._bid_size] + state[:, self._ask_size]
        x[:, 2 * k] = np.where(both & (sizes > 0),
                               (state[:, self._bid_size] - state[:, self._ask_size])
                               / np.maximum(sizes, 1), 0.0)
        x[:, 2 * k + 1] = np.where(both, np.tanh((ask - bid - 1) / 4), 1.0)
        x[:, 2 * k + 2] = state[:, self._position] / np.maximum(state[:, self._limit], 1)
        last = state[:, self._last].astype(float)
        x[:, 2 * k + 3] = np.where(both & (last > 0), np.tanh(np.nan_to_num(last - self.mid) / 2),
                                   0.0)
        x[:, -1] = 1.0
        return x


def orders(weights: np.ndarray, x: np.ndarray, state: np.ndarray, names: list[str], size: int,
           max_position: int) -> np.ndarray:
    """Each market's orders from its weights, (markets, heads, features), and its features: a
    take toward its limit on the side whose score is past 1, and a bid and an offer at the best
    prices where their scores are above zero."""
    z = np.einsum("mhf,mf->mh", weights, x)
    position = state[:, names.index("position")]
    bid, ask = state[:, names.index("bid_price")], state[:, names.index("ask_price")]
    # Toward the limit on a side while its score is past 1, and holding what it has between: the
    # generic takers' rule, on a score of all the features at once.
    target = np.where(z[:, 0] > 1, max_position, np.where(z[:, 0] < -1, -max_position, position))
    take = np.clip(target - position, -size, size)
    take = np.where(((take > 0) & (ask > 0)) | ((take < 0) & (bid > 0)), take, 0)
    out = np.zeros((len(x), 5), dtype=np.int64)
    # A take cancels the seat's quote on the side it takes from, a buy its offer and a sell its
    # bid, so none is asked for there.
    quote_bid = (z[:, 1] > 0) & (bid > 0) & (take >= 0)
    quote_ask = (z[:, 2] > 0) & (ask > 0) & (take <= 0)
    out[:, 0] = np.where(quote_bid, bid, 0)
    out[:, 1] = np.where(quote_bid, size, 0)
    out[:, 2] = np.where(quote_ask, ask, 0)
    out[:, 3] = np.where(quote_ask, size, 0)
    out[:, 4] = take
    return out


@dataclass
class Settings:
    """How the probe trades and trains."""

    latency: str = "1ms"
    size: int = 1
    max_position: int = 10
    interval: str = "1s"
    generations: int = 20
    markets: int = 16  # markets for each candidate, each generation
    sigma: float = 0.5
    threads: int = 0
    seed: int = 1


@dataclass
class Played:
    """What each market made, after fees, in tick-lots, and the lots it traded, as a maker and in
    all."""

    made: np.ndarray
    lots: np.ndarray
    maker: np.ndarray = field(default_factory=lambda: np.zeros(0))
    # What it made to its last decision, valued as the market marks a position then, at the true
    # value where it has one: all but the close, where a contract that settles jumps to its
    # outcome, unless the loss limit stopped the seat. What a calibrated value expects the same,
    # with less noise, and so what it trains on.
    marked: np.ndarray = field(default_factory=lambda: np.zeros(0))


def play(learn, texts: list[str], weights: np.ndarray, seeds: list[int], settings: Settings,
         pool_markets: int = 256) -> Played:
    """Plays job i, weights[i] on seeds[i] in texts[seeds[i] mod len], in a pool of markets
    refilled as episodes end: a seed runs the same scenario whichever candidate plays it."""
    jobs = len(seeds)
    count = max(1, min(pool_markets, jobs))
    pool = learn.Pool.from_text(texts[0], markets=count, threads=settings.threads)
    names = {"state": list(learn.STATE_NAMES)}
    features = Features(count, names)
    every = _nanoseconds(settings.interval)
    clock = names["state"].index("time")
    open_buy, open_sell = names["state"].index("open_buy"), names["state"].index("open_sell")
    last = np.zeros((count, 5), dtype=np.int64)
    following = 0  # the next job to start
    running = np.full(count, -1)  # each market's job, or -1 for none
    # What each job made, the lots it traded and those it rested, a row a job and one more, which
    # markets with nothing left to run add to.
    totals = np.zeros((jobs + 1, 4))
    w = np.zeros((count, len(HEADS), len(FEATURES)))

    def start(mask: np.ndarray) -> None:
        nonlocal following
        for market in np.flatnonzero(mask):
            running[market] = following if following < jobs else -1
            following += 1
        # Markets with nothing left to run replay the first job, and are not counted.
        index = np.where(running >= 0, running, 0)
        pool.reset(seeds=[seeds[k] for k in index], mask=mask,
                   scenarios=[texts[seeds[k] % len(texts)] for k in index])
        last[mask] = 0
        w[mask] = np.where((running[mask] >= 0)[:, None, None], weights[index[mask]], 0.0)
        features.reset(mask)

    start(np.ones(count, dtype=bool))
    observation = pool.observation
    while (running >= 0).any():
        x = features.update(observation)
        # It acts on its schedule only: at a decision a fill of its own brings between them, it
        # leaves the quotes it still has as they are, places none afresh and takes nothing, so
        # that it cannot trade on every fill, a decision apiece.
        state = observation["state"]
        scheduled = state[:, clock] % every == 0
        chosen = orders(w, x, state, names["state"], settings.size, settings.max_position)
        last[scheduled] = chosen[scheduled]
        sent = last.copy()
        between = ~scheduled
        sent[between, 4] = 0
        sent[between & (state[:, open_buy] <= 0), :2] = 0
        sent[between & (state[:, open_sell] <= 0), 2:4] = 0
        last[between] = sent[between]
        observation, rewards, terminated, _, info = pool.step_orders(sent)
        events, rows = _heard(observation)
        fills = rows & (events[:, :, 0] == _FILL)
        filled = np.where(fills, events[:, :, 4], 0).sum(axis=1)
        rested = np.where(fills & (events[:, :, 5] == 0), events[:, :, 4], 0).sum(axis=1)
        np.add.at(totals, np.where(running >= 0, running, jobs),
                  np.stack([rewards, filled, rested,
                            np.where(terminated & ~info["stopped"], 0.0, rewards)], axis=1))
        ended = terminated.copy()
        if ended.any():
            start(ended)
            observation = pool.observation
    return Played(*totals[:jobs].T)


def train(learn, texts: list[str], settings: Settings, log=print) -> tuple[np.ndarray, list]:
    """The weights CMA-ES settles on, from all zeros, a strategy that never trades, scored on
    what each candidate made to the last decision of each market; and each generation's mean of
    that a market, over its candidates."""
    shape = (len(HEADS), len(FEATURES))
    cma = Cma(np.zeros(int(np.prod(shape))), settings.sigma, settings.seed)
    history = []
    for generation in range(settings.generations):
        points = cma.ask()
        first = SEARCH_SEED + generation * settings.markets
        seeds = list(range(first, first + settings.markets)) * len(points)
        candidates = np.repeat(points.reshape(len(points), *shape), settings.markets, axis=0)
        played = play(learn, texts, candidates, seeds, settings)
        scores = played.marked.reshape(len(points), settings.markets).mean(axis=1)
        cma.tell(points, -scores)
        history.append(float(scores.mean()))
        log(f"learner, generation {generation + 1}: {scores.mean():+.1f} a market on average, "
            f"best {scores.max():+.1f}")
    return cma.mean.reshape(shape), history


def judge(learn, texts: list[str], weights: np.ndarray, confirm: int,
          settings: Settings) -> Played:
    seeds = list(range(CONFIRM_SEED, CONFIRM_SEED + confirm))
    return play(learn, texts, np.repeat(weights[None], confirm, axis=0), seeds, settings)


def summary(played: Played) -> dict:
    """As the sweep summarizes a probe, with the share of its lots it rested."""
    pnl, lots = played.made, played.lots
    mean = float(pnl.mean())
    error = float(pnl.std(ddof=1) / math.sqrt(len(pnl))) if len(pnl) > 1 else math.nan
    return {"mean": mean, "low": mean - 1.96 * error, "high": mean + 1.96 * error,
            "per_lot": float(pnl.sum() / lots.sum()) if lots.sum() > 0 else math.nan,
            "lots": float(lots.mean()), "markets": len(pnl),
            "maker_share": float(played.maker.sum() / lots.sum()) if lots.sum() > 0 else math.nan}


def weights_table(weights: np.ndarray) -> str:
    """The trained weights as Markdown: which signals the learner trades on."""
    lines = ["| feature | " + " | ".join(HEADS) + " |", "|---|" + "---:|" * len(HEADS)]
    for f, name in enumerate(FEATURES):
        lines.append(f"| {name} | " + " | ".join(f"{weights[h, f]:+.2f}"
                                                 for h in range(len(HEADS))) + " |")
    return "\n".join(lines) + "\n"
