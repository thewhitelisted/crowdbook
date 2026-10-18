"""A strategy run in many markets at once, and what it did there: what it made, its fills, what
its fills were worth a while later, and how that holds up as it trades bigger.

    from crowdbook_learn.evaluate import evaluate

    def policy(observation, info):            # every market's row at once
        ...                                    # returns a row of five numbers a market
    result = evaluate(policy, scenario_text, seeds=range(1, 401), scales=(1, 2, 4))

The policy answers each decision with orders, as `Pool.step_orders` takes them: the bid's price
and size, the offer's price and size, and a quantity to take, negative to sell. Markets come from
one scenario or from several, such as draws from a fit's uncertainty, one per seed in turn, so
that a strategy is judged across what the fit allows rather than at its point alone. At each
scale every size the policy asks for is multiplied by the scale, and the same seeds run again:
where what it makes a lot falls as it grows, the market's depth is its capacity.
"""

from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

import numpy as np

from crowdbook_learn import EVENT_NAMES, STATE_NAMES, Pool

_TIME = STATE_NAMES.index("time")
_BID = STATE_NAMES.index("bid_price")
_ASK = STATE_NAMES.index("ask_price")
_FILL = EVENT_NAMES.index("fill")
_QUOTE = EVENT_NAMES.index("quote")
_ACCEPTED = EVENT_NAMES.index("accepted")


@dataclass
class Episode:
    """One market as the strategy traded it."""

    seed: int
    scenario: int  # which of the scenarios it ran
    score: float = 0.0  # tick-lots, positions closed as the episode values them
    fills: list = field(default_factory=list)  # (time, side, price, quantity, maker, fee units)
    mids: list = field(default_factory=list)  # (time, mid) as the seat heard them
    value: float = float("nan")  # the true value at the end, where there is one
    # Quotes as they reached the book: (time, side, price, quantity, lots ahead), the lots ahead
    # being the size the seat last saw at that price, its own order not yet there.
    quotes: list = field(default_factory=list)


def _ahead(book: np.ndarray, side: int, price: int) -> int:
    """The size at `price` on the seat's side of the book it last saw: book[0] bids, book[1]
    offers, each levels of (price, quantity)."""
    levels = book[0 if side > 0 else 1]
    match = levels[levels[:, 0] == price]
    return int(match[0, 1]) if len(match) else 0


def _record(episode: Episode, row_state: np.ndarray, events: np.ndarray,
            book: np.ndarray | None = None) -> None:
    for event in events:
        kind, time = int(event[0]), int(event[1])
        if kind == _ACCEPTED and int(event[5]) == 0 and book is not None:
            side, price = int(event[2]), int(event[3])
            episode.quotes.append((time, side, price, int(event[4]), _ahead(book, side, price)))
        elif kind == _FILL:
            episode.fills.append((time, int(event[2]), int(event[3]), int(event[4]),
                                  int(event[5]) == 0, int(event[6])))
        elif kind == _QUOTE and event[2] > 0 and event[4] > 0:
            episode.mids.append((time, (event[2] + event[4]) / 2))
    if row_state[_BID] > 0 and row_state[_ASK] > 0:
        episode.mids.append((int(row_state[_TIME]), (row_state[_BID] + row_state[_ASK]) / 2))


def run(policy: Callable, scenarios: str | Sequence[str], seeds: Sequence[int], *,
        markets: int = 32, threads: int = 0, scale: int = 1) -> list[Episode]:
    """Every seed's episode, in the order of the seeds: seed i runs scenarios[i % len], and the
    policy's sizes are multiplied by `scale`."""
    texts = [scenarios] if isinstance(scenarios, str) else list(scenarios)
    seeds = list(seeds)
    pool = Pool.from_text(texts[0], markets=min(markets, len(seeds)), threads=threads)
    count = pool.markets
    queue = list(range(len(seeds)))
    done: dict[int, Episode] = {}
    running: list[int | None] = [None] * count

    def start(mask: np.ndarray) -> None:
        chosen = []
        for market in range(count):
            if mask[market]:
                running[market] = queue.pop(0) if queue else None
            chosen.append(running[market])
        # Markets with nothing left to run replay their last seed, and are not recorded.
        index = [k if k is not None else 0 for k in chosen]
        pool.reset(seeds=[seeds[k] for k in index], mask=mask,
                   scenarios=[texts[k % len(texts)] for k in index])
        for market in range(count):
            if mask[market] and running[market] is not None:
                k = running[market]
                episodes[market] = Episode(seed=seeds[k], scenario=k % len(texts))

    episodes: list[Episode | None] = [None] * count
    start(np.ones(count, dtype=bool))
    scores = np.zeros(count)
    while any(k is not None for k in running):
        seen = pool.observation["book"].copy()
        orders = np.asarray(policy(pool.observation, pool.info), dtype=np.int64).copy()
        orders[:, [1, 3, 4]] *= scale
        observation, rewards, terminated, _, info = pool.step_orders(orders)
        scores += rewards
        for market in range(count):
            if running[market] is None:
                continue
            kept = min(int(observation["counts"][market][0]), len(observation["events"][market]))
            _record(episodes[market], observation["state"][market],
                    observation["events"][market][:kept], seen[market])
        ended = terminated.copy()
        if ended.any():
            for market in np.flatnonzero(ended):
                if running[market] is not None:
                    episodes[market].score = float(scores[market])
                    episodes[market].value = float(info["value"][market])
                    done[running[market]] = episodes[market]
                scores[market] = 0.0
            start(ended)
    return [done[k] for k in range(len(seeds))]


def markouts(episode: Episode, horizons: Sequence[float]) -> np.ndarray:
    """For each fill and horizon, what a unit was worth that long after, against the mid the seat
    heard then: positive where the fill was good for the seat. Rows of fills, columns of horizons
    in seconds."""
    if not episode.fills or not episode.mids:
        return np.empty((0, len(horizons)))
    times = np.array([t for t, _ in episode.mids], dtype=float)
    mids = np.array([m for _, m in episode.mids], dtype=float)
    order = np.argsort(times, kind="stable")
    times, mids = times[order], mids[order]
    rows = []
    for time, side, price, _quantity, _maker, _fee in episode.fills:
        row = []
        for horizon in horizons:
            at = np.searchsorted(times, time + horizon * 1e9, side="right") - 1
            row.append(side * (mids[at] - price) if at >= 0 else np.nan)
        rows.append(row)
    return np.array(rows)


def summarize(episodes: list[Episode], horizons: Sequence[float] = (1, 10, 60)) -> dict:
    """What the strategy made a market, with a 95% interval; the lots it traded, as a maker and as
    a taker; what it made a lot; its quotes' place in the queue (the lots ahead of each when it
    reached the book, as the size at its price the seat last saw), the share of the lots it quoted
    that filled and how long a filled quote waited; and its fills' markouts, size-weighted, by
    horizon."""
    scores = np.array([e.score for e in episodes])
    lots = np.array([sum(f[3] for f in e.fills) for e in episodes], dtype=float)
    maker = np.array([sum(f[3] for f in e.fills if f[4]) for e in episodes], dtype=float)
    weighted = np.zeros(len(horizons))
    weights = np.zeros(len(horizons))
    for e in episodes:
        marks = markouts(e, horizons)
        if len(marks):
            sizes = np.array([f[3] for f in e.fills], dtype=float)[:, None]
            ok = np.isfinite(marks)
            weighted += np.nansum(marks * sizes, axis=0)
            weights += np.sum(sizes * ok, axis=0)
    quoted = sum(q[3] for e in episodes for q in e.quotes)
    ahead = [q[4] for e in episodes for q in e.quotes]
    waits = []
    for e in episodes:
        # A maker fill's wait: from the latest quote on its side at its price before it.
        for time, side, price, _quantity, rested, _fee in e.fills:
            if rested:
                placed = [q[0] for q in e.quotes if q[1] == side and q[2] == price and q[0] <= time]
                if placed:
                    waits.append((time - max(placed)) / 1e9)
    error = scores.std(ddof=1) / np.sqrt(len(scores)) if len(scores) > 1 else np.nan
    return {
        "markets": len(episodes),
        "made": float(scores.mean()),
        "made_low": float(scores.mean() - 1.96 * error),
        "made_high": float(scores.mean() + 1.96 * error),
        "lots": float(lots.mean()),
        "maker_share": float(maker.sum() / lots.sum()) if lots.sum() > 0 else float("nan"),
        "made_per_lot": float(scores.sum() / lots.sum()) if lots.sum() > 0 else float("nan"),
        "quote_fill_rate": float(maker.sum() / quoted) if quoted > 0 else float("nan"),
        "lots_ahead_median": float(np.median(ahead)) if ahead else float("nan"),
        "wait_to_fill_median": float(np.median(waits)) if waits else float("nan"),
        "markouts": {f"{h:g}s": float(w / n) if n > 0 else float("nan")
                     for h, w, n in zip(horizons, weighted, weights, strict=True)},
    }


def evaluate(policy: Callable, scenarios: str | Sequence[str], seeds: Sequence[int], *,
             scales: Sequence[int] = (1,), horizons: Sequence[float] = (1, 10, 60),
             markets: int = 32, threads: int = 0) -> dict:
    """The strategy at each scale, on the same seeds: a summary each, and the episodes."""
    out = {}
    for scale in scales:
        episodes = run(policy, scenarios, seeds, markets=markets, threads=threads, scale=scale)
        out[scale] = {"summary": summarize(episodes, horizons), "episodes": episodes}
    return out


def per_market(decide: Callable) -> Callable:
    """A policy from a function that decides for one market at a time: decide(state, events,
    book, orders) gets that market's arrays and returns its five numbers."""
    def policy(observation, info):
        rows = []
        for market in range(len(observation["state"])):
            kept = min(int(observation["counts"][market][0]), len(observation["events"][market]))
            rows.append(decide(observation["state"][market],
                               observation["events"][market][:kept],
                               observation["book"][market], observation["orders"][market]))
        return np.array(rows, dtype=np.int64)
    return policy
