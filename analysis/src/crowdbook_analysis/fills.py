"""A strategy's paper fills set beside its fills in a fitted market: whether the fit predicts how
the strategy trades in the real one.

    from crowdbook_analysis import fills
    from crowdbook_learn.evaluate import run
    paper = fills.read_paper("paper/")                       # what a paper trader recorded
    twins = run(policy, Path("fit/twins"), seeds=range(1, 401))
    print(fills.compare(paper, twins))

A paper trader watches a real book and records what its strategy would have done there: the
quotes it would have placed and the fills it would have had. Run the same strategy in the fitted
market, through the learning pool's seat (`crowdbook_learn.evaluate.run`), and the two give the
same figures, measured the same way by `crowdbook_learn.evaluate.summarize`: the share of quoted
lots that filled, the lots ahead of a quote when it reached the book, how long a filled quote
waited, the share of fills made resting, what a fill was worth 1, 10 and 60 seconds later
against the mid, and the lots a market. Each comes with a 95% interval from resampling markets,
on both sides; a figure whose intervals do not overlap is one the fit gets wrong for this
strategy, and the strategy's results in the fit should not be trusted until it is explained.

A paper record is a tape (validate.md, "Tapes"), the book the strategy watched, with two more
tables beside it, CSV or Parquet, times in seconds from each market's start and prices in the
tape's units:

- `quotes`: `market`, `time`, `side` (`buy` or `sell`), `price`, `size`, and optionally `ahead`,
  the size the book showed at that price before the quote joined it: each quote placed or moved;
- `fills`: `market`, `time`, `side`, `price`, `quantity`, `liquidity` (`maker` or `taker`): each
  fill, whole or partial.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import polars as pl

from crowdbook_analysis import tape as tapes

NANOSECONDS = 1e9
FIGURES = (("quote_fill_rate", "quoted lots that filled", "share"),
           ("lots_ahead_median", "lots ahead of a quote, median", "lots"),
           ("wait_to_fill_median", "wait to fill, median", "seconds"),
           ("maker_share", "fills made resting", "share"),
           ("lots", "lots traded a market", "lots"))


@dataclass
class PaperEpisode:
    """One market as a paper trader traded it, with the fields of the strategy harness's
    episodes that its summary reads, in its units: nanoseconds and ticks."""

    seed: int
    scenario: int
    score: float = 0.0
    fills: list = field(default_factory=list)  # (time, side, price, quantity, maker, fee units)
    mids: list = field(default_factory=list)  # (time, mid)
    value: float = float("nan")
    quotes: list = field(default_factory=list)  # (time, side, price, quantity, lots ahead)


def harness_summary(episodes: list) -> dict:
    """crowdbook_learn.evaluate.summarize, where the learning probe finds crowdbook_learn."""
    from crowdbook_analysis.learner import learn_module
    learn_module()
    from crowdbook_learn.evaluate import summarize
    return summarize(episodes)


def _side(values) -> np.ndarray:
    text = np.asarray([str(v).lower() for v in values])
    if not np.isin(text, ["buy", "sell"]).all():
        raise ValueError("a side is buy or sell")
    return np.where(text == "buy", 1, -1)


def read_paper(directory: Path | str) -> list:
    """A paper record as episodes like the strategy harness's, one a market of its tape, prices
    in ticks and times in nanoseconds as the harness records them."""
    directory = Path(directory)
    recorded = tapes.read_tape(directory)
    tick = recorded.options.tick
    quotes = tapes._table(directory, "quotes")
    fill_rows = tapes._table(directory, "fills")
    episodes = []
    for index, m in enumerate(recorded.markets):
        episode = PaperEpisode(seed=index, scenario=index)
        mid = (m.bid_price[:, 0] + m.ask_price[:, 0]) / 2 / tick
        kept = np.isfinite(mid)
        episode.mids = list(zip((m.book_time[kept] * NANOSECONDS).astype(np.int64).tolist(),
                                mid[kept].tolist(), strict=True))
        mine = quotes.filter(pl.col("market") == m.name)
        if len(mine):
            ahead = (mine["ahead"].to_numpy() if "ahead" in mine.columns
                     else np.zeros(len(mine)))
            episode.quotes = [
                (int(t * NANOSECONDS), int(s), int(round(p / tick)), int(q), int(a))
                for t, s, p, q, a in zip(mine["time"].to_numpy(), _side(mine["side"]),
                                         mine["price"].to_numpy(), mine["size"].to_numpy(),
                                         ahead, strict=True)]
        mine = fill_rows.filter(pl.col("market") == m.name)
        if len(mine):
            liquidity = [str(v).lower() for v in mine["liquidity"]]
            if not set(liquidity) <= {"maker", "taker"}:
                raise ValueError("a fill's liquidity is maker or taker")
            episode.fills = [
                (int(t * NANOSECONDS), int(s), int(round(p / tick)), int(q), kind == "maker", 0)
                for t, s, p, q, kind in zip(mine["time"].to_numpy(), _side(mine["side"]),
                                            mine["price"].to_numpy(),
                                            mine["quantity"].to_numpy(), liquidity, strict=True)]
        episodes.append(episode)
    return episodes


def _figures(summary: dict) -> dict[str, float]:
    out = {key: summary[key] for key, _, _ in FIGURES}
    for horizon, value in summary["markouts"].items():
        out[f"markout_{horizon}"] = value
    return out


def summarize(episodes: list, draws: int = 200, seed: int = 1,
              measure=harness_summary) -> dict[str, tuple]:
    """Each figure of the episodes, with the 2.5th and 97.5th percentiles over resamples of
    them: {figure: (value, low, high)}. `measure` summarizes a list of episodes, the strategy
    harness's summary unless given."""
    summarize_episodes = measure
    point = _figures(summarize_episodes(episodes))
    rng = np.random.default_rng(seed)
    samples = {key: [] for key in point}
    for _ in range(draws):
        chosen = [episodes[i] for i in rng.integers(0, len(episodes), len(episodes))]
        for key, value in _figures(summarize_episodes(chosen)).items():
            samples[key].append(value)
    out = {}
    for key, value in point.items():
        found = np.asarray(samples[key], dtype=float)
        found = found[np.isfinite(found)]
        low, high = (np.percentile(found, [2.5, 97.5]) if len(found) else (math.nan, math.nan))
        out[key] = (float(value), float(low), float(high))
    return out


def _label(key: str) -> tuple[str, str]:
    for name, label, unit in FIGURES:
        if name == key:
            return label, unit
    horizon = key.removeprefix("markout_")
    return f"a fill's worth {horizon} later, against the mid", "ticks"


def compare(paper: list, simulated: list, *, draws: int = 200,
            measure=harness_summary) -> tuple[str, list[str]]:
    """The paper trader's figures beside the fitted market's, as Markdown, and the figures whose
    intervals do not overlap: where the fit trades the strategy unlike the real market."""
    real = summarize(paper, draws, measure=measure)
    fitted = summarize(simulated, draws, seed=2, measure=measure)
    lines = [f"Paper trading on {len(paper)} markets against the fitted market's "
             f"{len(simulated)}, each figure with its 95% interval over resamples of markets.",
             "", "| figure | paper | fitted market | |", "|---|---:|---:|---|"]
    apart = []
    for key, (value, low, high) in real.items():
        other, other_low, other_high = fitted[key]
        label, unit = _label(key)
        overlap = not (high < other_low or other_high < low)
        known = all(math.isfinite(x) for x in (low, high, other_low, other_high))
        if known and not overlap:
            apart.append(key)
        lines.append(f"| {label} ({unit}) | {_cell(value, low, high)} | "
                     f"{_cell(other, other_low, other_high)} | "
                     f"{'' if not known else 'apart' if not overlap else ''} |")
    lines += ["", ("**The fit trades this strategy unlike the real market**: "
                   + ", ".join(_label(k)[0] for k in apart) + "." if apart else
                   "**Every figure's intervals overlap**: the fit trades this strategy as the "
                   "real market did, as far as these figures and markets can tell.")]
    return "\n".join(lines) + "\n", apart


def _cell(value: float, low: float, high: float) -> str:
    if not math.isfinite(value):
        return "–"
    return f"{value:.3g} [{low:.3g}, {high:.3g}]" if math.isfinite(low) else f"{value:.3g}"


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paper", type=Path, help="a paper record: a tape with quotes and fills")
    args = parser.parse_args()
    figures = summarize(read_paper(args.paper))
    for key, (value, low, high) in figures.items():
        label, unit = _label(key)
        print(f"{label} ({unit}): {_cell(value, low, high)}")
    sys.exit(0)


if __name__ == "__main__":
    main()
