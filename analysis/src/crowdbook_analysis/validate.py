"""A validation report: a synthetic market's statistics set beside a real market's, each with an
interval and a distance, so that anyone can see where the synthetic market is the real one and
where it is not.

    uv run --project analysis crowdbook-validate --real REAL_TAPE --sim SIMULATED_TAPE
    uv run --project analysis crowdbook-validate --real REAL_TAPE \\
        --scenario examples/scenarios/up_or_down.toml --seeds 100001:100401

Both sides are tapes (see `tape`): a directory of book snapshots, prints and, optionally, a
reference value and how each market settled. `--scenario` runs a scenario into a tape first. The
statistics, measured alike on both:

- **The book at the touch**: the spread, time-weighted, its median and how often it is one tick;
  the size at the best bid and ask, and within the first five levels; how often one side is empty.
- **Trades**: aggressive orders a minute (the prints one order makes at one time are one order),
  their sizes (the median, the largest tenth against the median and the tail's Hill exponent),
  how bursty they come (the coefficient of variation of the gaps between them), and the share in
  the first and last tenth of the market's life.
- **Returns**: the mid's changes over one, ten and sixty grid steps (a second each by default),
  their standard deviation, excess kurtosis and the Hill exponent of the tail, the variance ratio
  (below one where prices bounce, above where they trend) and the autocorrelation of successive
  changes.
- **Volatility clustering**: the autocorrelation of the size of one-step changes, one and ten steps
  apart.
- **Order-flow memory**: the autocorrelation of aggressive orders' signs one, ten and fifty orders
  apart, and of successive changes in trade price (the bounce, from which Roll's spread).
- **What resting orders earn**: for every print, what the resting side made per unit, from the mid
  just before (the effective half spread) and marked to the mid one, ten and sixty seconds later,
  and at settlement where the market settled: the markouts of makers' fills, size-weighted.
- **How fast the mid follows the value**, where there is one: of a move of the value over one
  second, the share the mid has made 0, 0.5, 1, 2 and 5 seconds after it ends; the delay at which
  the mid is closest to the value as it was; and how far it is from the value.
- **Calibration**, for a binary contract that settled: the Brier score of the mid as a
  probability, and how often markets priced 10 to 20% and 80 to 90% of the payout paid out.

Prices are counted in ticks, the tape's `tick`, so that a market priced in dollars and one in
cents compare. Every statistic is pooled over markets, and its interval comes from resampling markets with
replacement (200 times by default), the unit that is independent. The distance of a synthetic
statistic from the real one is their difference over the two resampling spreads together, in
those tolerances, so that 2 is about the edge of chance; rates, sizes and spreads are compared as
logarithms. Distributions are compared too: the Kolmogorov–Smirnov distance between the pooled
spreads, sizes at the touch, order sizes and one-step changes, beside the distance between two
random halves of the real markets, which is what chance alone gives.
"""

import argparse
import json
import math
from dataclasses import dataclass, replace
from pathlib import Path

import numpy as np

from crowdbook_analysis import tape as tapes
from crowdbook_analysis.facts import sample_last
from crowdbook_analysis.runner import find_tool


@dataclass(frozen=True)
class Config:
    grid: float = 1.0  # seconds between samples of the mid, for returns
    horizons: tuple = (1, 10, 60)  # grid steps
    clustering_lags: tuple = (1, 10)  # grid steps
    sign_lags: tuple = (1, 10, 50)  # orders
    markouts: tuple = (1.0, 10.0, 60.0)  # seconds
    move: float = 1.0  # seconds: the value's moves the mid is set to follow
    follow_lags: tuple = (0.0, 0.5, 1.0, 2.0, 5.0)  # seconds after a move ends
    max_delay: float = 60.0  # seconds: the longest lag behind the value looked for
    delay_step: float = 0.5
    levels: int = 5  # book levels counted for depth
    tail: float = 0.1  # the share of the largest counted as a tail
    bootstrap: int = 200


# Every statistic: its key, how the report names it, and whether it is compared as a logarithm.
# Some exist only where a tape has them: a value, an outcome, a payout.
def statistic_names(config: Config = Config()) -> list[tuple[str, str, bool]]:
    names = [
        ("spread_mean", "spread, time-weighted mean (ticks)", True),
        ("spread_median", "spread, median (ticks)", True),
        ("spread_one_tick", "spread one tick, share of time", False),
        ("one_sided", "one side of the book empty, share of time", False),
        ("touch_size_median", "size at the touch, median", True),
        ("touch_size_mean", "size at the touch, mean", True),
        ("depth_mean", f"size within {config.levels} levels a side, mean", True),
        ("depth_near_3", "size within 3 ticks of the mid a side, mean", True),
        ("depth_near_10", "size within 10 ticks of the mid a side, mean", True),
        ("touch_turnover", "lots traded a minute over the size at the touch", True),
        ("orders_per_minute", "aggressive orders a minute", True),
        ("prints_per_order", "prints per aggressive order", True),
        ("order_size_median", "order size, median", True),
        ("order_size_tail", "order size, largest tenth against the median", True),
        ("order_size_hill", "order size, tail exponent", True),
        ("order_gap_median", "gaps between orders, median (seconds)", True),
        ("order_gap_cv", "gaps between orders, coefficient of variation", True),
        ("orders_first_tenth", "orders in the first tenth of the market", False),
        ("orders_last_tenth", "orders in the last tenth of the market", False),
    ]
    for h in config.horizons:
        seconds = f"{h * config.grid:g}s"
        names.append((f"return_sd_{h}", f"mid change over {seconds}, standard deviation", True))
        names.append((f"return_kurtosis_{h}", f"mid change over {seconds}, excess kurtosis",
                      False))
    first = config.horizons[0]
    names.append((f"return_hill_{first}", f"mid change over {first * config.grid:g}s, "
                  "tail exponent", True))
    for h in config.horizons[1:]:
        names.append((f"variance_ratio_{h}", f"variance ratio, {h * config.grid:g}s to "
                      f"{first * config.grid:g}s", True))
    names.append(("return_autocorrelation", "successive mid changes, autocorrelation", False))
    for lag in config.clustering_lags:
        names.append((f"clustering_{lag}", f"size of mid changes, autocorrelation {lag} "
                      f"step{'s' if lag > 1 else ''} apart", False))
    for lag in config.sign_lags:
        names.append((f"sign_memory_{lag}", f"order signs, {lag} apart", False))
    names += [
        ("bounce", "successive trade-price changes, autocorrelation", False),
        ("roll_spread", "spread implied by the bounce (ticks)", True),
        ("maker_at_trade", "resting orders earn, at the trade (ticks)", False),
    ]
    for h in config.markouts:
        names.append((f"maker_after_{h:g}", f"resting orders earn, marked {h:g}s later", False))
    names.append(("maker_at_settlement", "resting orders earn, at settlement", False))
    for lag in config.follow_lags:
        names.append((f"follow_{lag:g}", f"mid's share of a {config.move:g}s value move, "
                      f"{lag:g}s after", False))
    names += [
        ("delay", "mid closest to the value as it was (seconds before)", False),
        ("value_distance", "mean distance from the value (ticks)", True),
        ("touch_through_value", "best bid above or best ask below the value, share of time",
         False),
        ("touch_through_value_2", "the same, by more than 2 ticks", False),
        ("score_touch_median", "a score to the touch moving, median (seconds)", True),
        ("score_touch_within_5", "a score to the touch moving, share within 5 s", False),
        ("score_value_median", "a score to the value moving, median (seconds)", True),
        ("brier", "Brier score of the mid", False),
        ("priced_low", "priced 10 to 20%: paid out", False),
        ("priced_high", "priced 80 to 90%: paid out", False),
    ]
    return names


# -- per-market pieces: sums and samples that pool across markets ----------------------------

def _autocorrelation_sums(x: np.ndarray, lags) -> np.ndarray:
    """For pooling an autocorrelation across markets: n, Σx, Σx², then for each lag the number
    of pairs, Σ x_t x_{t+lag}, Σ x_t over the first members and Σ x_{t+lag} over the second."""
    x = x[np.isfinite(x)]
    row = [len(x), x.sum(), (x * x).sum()]
    for lag in lags:
        if len(x) > lag:
            row += [len(x) - lag, float(x[:-lag] @ x[lag:]), x[:-lag].sum(), x[lag:].sum()]
        else:
            row += [0, 0.0, 0.0, 0.0]
    return np.array(row, dtype=float)


def _pooled_autocorrelation(sums: np.ndarray, lags) -> np.ndarray:
    total = sums.sum(axis=0)
    n, sx, sxx = total[:3]
    if n < 3:
        return np.full(len(lags), np.nan)
    mean = sx / n
    variance = sxx / n - mean * mean
    out = []
    for i in range(len(lags)):
        pairs, sxy, head, tail = total[3 + 4 * i: 7 + 4 * i]
        if pairs < 2 or variance <= 0:
            out.append(np.nan)
            continue
        covariance = (sxy - mean * (head + tail) + pairs * mean * mean) / pairs
        out.append(covariance / variance)
    return np.array(out)


def _moments(x: np.ndarray) -> np.ndarray:
    x = x[np.isfinite(x)]
    return np.array([len(x), x.sum(), (x**2).sum(), (x**3).sum(), (x**4).sum()], dtype=float)


def _pooled_kurtosis(moments: np.ndarray) -> tuple[float, float]:
    """The standard deviation and excess kurtosis of pooled samples, from their power sums."""
    n, s1, s2, s3, s4 = moments.sum(axis=0)
    if n < 4:
        return np.nan, np.nan
    m = s1 / n
    variance = s2 / n - m * m
    if variance <= 0:
        return 0.0, np.nan
    fourth = s4 / n - 4 * m * s3 / n + 6 * m * m * s2 / n - 3 * m**4
    return math.sqrt(variance), fourth / variance**2 - 3


def hill(x: np.ndarray, share: float) -> float:
    """Hill's estimate (1975) of the tail exponent of positive samples, from the largest `share`
    of them."""
    x = np.sort(x[np.isfinite(x) & (x > 0)])
    k = int(len(x) * share)
    if k < 5:
        return np.nan
    threshold = x[-k - 1]
    if threshold <= 0:
        return np.nan
    logs = np.log(x[-k:] / threshold)
    mean = logs.mean()
    return 1 / mean if mean > 0 else np.nan


def _weighted_quantile(values: np.ndarray, weights: np.ndarray, q: float) -> float:
    if len(values) == 0 or weights.sum() <= 0:
        return np.nan
    order = np.argsort(values)
    cumulative = np.cumsum(weights[order])
    return float(values[order][np.searchsorted(cumulative, q * cumulative[-1])])


def _orders(m: tapes.Market) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Aggressive orders from prints: the prints at one time on one side are one order, at their
    size-weighted price. Returns their times, sizes, signs and prices."""
    if len(m.trade_time) == 0:
        empty = np.array([])
        return empty, empty, empty, empty
    new = np.ones(len(m.trade_time), dtype=bool)
    new[1:] = (m.trade_time[1:] != m.trade_time[:-1]) | (m.trade_sign[1:] != m.trade_sign[:-1])
    index = np.cumsum(new) - 1
    sizes = np.bincount(index, weights=m.trade_size)
    notional = np.bincount(index, weights=m.trade_size * m.trade_price)
    with np.errstate(invalid="ignore", divide="ignore"):
        prices = notional / sizes
    return m.trade_time[new], sizes, m.trade_sign[new], prices


def _mid(m: tapes.Market) -> np.ndarray:
    return (m.bid_price[:, 0] + m.ask_price[:, 0]) / 2


def _changes(x: np.ndarray, returns: str) -> np.ndarray:
    if returns == "log":
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.diff(np.log(np.where(x > 0, x, np.nan)))
    return np.diff(x)


def _reactions(events: np.ndarray, times: np.ndarray, series: np.ndarray,
               within: float = 300.0) -> np.ndarray:
    """For each event, how long until the series changes from what it was at the event, in
    seconds; NaN where it does not within `within`."""
    out = np.full(len(events), np.nan)
    for i, t in enumerate(events):
        before = np.searchsorted(times, t, side="right") - 1
        if before < 0:
            continue
        then = series[before]
        later = np.arange(before + 1, min(np.searchsorted(times, t + within, side="right"),
                                          len(times)))
        moved = later[np.any((series[later] != then) & ~(np.isnan(series[later])
                                                         & np.isnan(then)), axis=1)]
        if len(moved):
            out[i] = times[moved[0]] - t
    return out


def market_pieces(m: tapes.Market, options: tapes.Options, config: Config) -> dict:
    """Everything one market adds to the pooled statistics. Prices are counted in ticks, so that
    markets priced in dollars and in cents compare."""
    if options.tick != 1.0:
        scale = 1.0 / options.tick
        m = replace(m, bid_price=m.bid_price * scale, ask_price=m.ask_price * scale,
                    trade_price=m.trade_price * scale,
                    outcome=None if m.outcome is None else m.outcome * scale,
                    value=None if m.value is None else m.value * scale)
        options = replace(options, tick=1.0,
                          payout=None if options.payout is None else options.payout * scale)
    p: dict = {"duration": m.duration}
    mid = _mid(m)
    # The book: each snapshot holds until the next or the end.
    held = np.diff(np.append(m.book_time, max(m.duration, m.book_time[-1] if len(m.book_time)
                                              else 0.0)))
    held = np.clip(held, 0.0, None)
    spread = m.ask_price[:, 0] - m.bid_price[:, 0]
    both = np.isfinite(spread)
    p["spread"], p["spread_weight"] = spread[both], held[both]
    p["one_sided"] = np.array([held[~both].sum(), held.sum()])
    touch = (m.bid_size[:, 0] + m.ask_size[:, 0]) / 2
    p["touch"], p["touch_weight"] = touch[both], held[both]
    levels = min(config.levels, m.bid_size.shape[1])
    depth = (np.nansum(m.bid_size[:, :levels], axis=1)
             + np.nansum(m.ask_size[:, :levels], axis=1)) / 2
    p["depth"] = np.array([float(depth[both] @ held[both]), float(held[both].sum())])
    for ticks in (3, 10):
        near = (np.nansum(np.where(np.abs(m.bid_price - mid[:, None]) <= ticks, m.bid_size, 0.0),
                          axis=1)
                + np.nansum(np.where(np.abs(m.ask_price - mid[:, None]) <= ticks, m.ask_size,
                                     0.0), axis=1)) / 2
        p[f"near_{ticks}"] = np.array([float(near[both] @ held[both]),
                                       float(held[both].sum())])
    p["volume"] = float(m.trade_size.sum())

    # Orders.
    times, sizes, signs, prices = _orders(m)
    p["orders"] = np.array([len(times), len(m.trade_time)])
    p["sizes"] = sizes
    gaps = np.diff(times)
    p["gaps"] = np.array([len(gaps), gaps.sum(), (gaps**2).sum()])
    p["gap_values"] = gaps
    share = times / m.duration if m.duration > 0 else times
    p["tenths"] = np.array([np.sum(share < 0.1), np.sum(share >= 0.9), len(times)])
    p["signs"] = _autocorrelation_sums(signs, config.sign_lags)
    price_changes = np.diff(prices)
    p["bounce"] = _autocorrelation_sums(price_changes, (1,))
    p["bounce_moments"] = _moments(price_changes)

    # Returns of the mid on the grid.
    grid = np.arange(0.0, m.duration + 1e-9, config.grid)
    sampled = sample_last(m.book_time, mid, grid) if len(m.book_time) else grid * np.nan
    one = _changes(sampled, options.returns)
    for h in config.horizons:
        r = _changes(sampled[::h], options.returns)
        p[f"returns_{h}"] = _moments(r)
        if h == config.horizons[0]:
            p["returns_first"] = r[np.isfinite(r)]
    p["return_autocorrelation"] = _autocorrelation_sums(one, (1,))
    p["clustering"] = _autocorrelation_sums(np.abs(one), config.clustering_lags)
    p["spread_grid"] = (sample_last(m.book_time, spread, grid) if len(m.book_time)
                        else grid * np.nan)
    p["touch_grid"] = sample_last(m.book_time, touch, grid) if len(m.book_time) else grid * np.nan

    # What resting orders earn on each print: the resting side sold where a buyer took.
    def mid_at(t: np.ndarray, strictly_before: bool) -> np.ndarray:
        index = np.searchsorted(m.book_time, t, side="left" if strictly_before else "right") - 1
        out = np.full(len(t), np.nan)
        ok = index >= 0
        out[ok] = mid[index[ok]]
        return out

    s, price, size = m.trade_sign, m.trade_price, m.trade_size
    earned = {"at_trade": s * (price - mid_at(m.trade_time, True))}
    for h in config.markouts:
        later = np.minimum(m.trade_time + h, m.duration)
        earned[f"after_{h:g}"] = s * (price - mid_at(later, False))
    if m.outcome is not None:
        earned["settlement"] = s * (price - m.outcome)
    p["maker"] = {k: np.array([np.nansum(v * size * np.isfinite(v)),
                               np.sum(size * np.isfinite(v))]) for k, v in earned.items()}

    # How the mid follows the value.
    if m.value is not None and len(m.value):
        longest = max(config.follow_lags)
        ends = np.arange(config.move, m.duration - longest + 1e-9, config.move)
        moved = sample_last(m.value_time, m.value, ends) - sample_last(
            m.value_time, m.value, ends - config.move)
        start = sample_last(m.book_time, mid, ends - config.move)
        follow = []
        for lag in config.follow_lags:
            response = sample_last(m.book_time, mid, ends + lag) - start
            ok = np.isfinite(moved) & np.isfinite(response)
            follow.append([float(moved[ok] @ response[ok]), float(moved[ok] @ moved[ok])])
        p["follow"] = np.array(follow)
        delays = np.arange(0.0, config.max_delay + 1e-9, config.delay_step)
        at = np.arange(config.max_delay, m.duration, config.grid)
        now = sample_last(m.book_time, mid, at)
        rows = []
        for d in delays:
            gap = np.abs(now - sample_last(m.value_time, m.value, at - d))
            ok = np.isfinite(gap)
            rows.append([gap[ok].sum(), ok.sum()])
        p["lateness"] = np.array(rows)

    # The touch against the value, and how soon the touch and the value move after a score.
    if m.value is not None and len(m.value) and len(m.book_time):
        value_now = sample_last(m.value_time, m.value, grid)
        bid_now = sample_last(m.book_time, m.bid_price[:, 0], grid)
        ask_now = sample_last(m.book_time, m.ask_price[:, 0], grid)
        known = np.isfinite(value_now) & (np.isfinite(bid_now) | np.isfinite(ask_now))
        through = np.fmax(np.nan_to_num(bid_now - value_now, nan=-np.inf),
                          np.nan_to_num(value_now - ask_now, nan=-np.inf))
        p["through"] = np.array([np.sum(known & (through > 0)), np.sum(known & (through > 2)),
                                 np.sum(known)], dtype=float)
    if m.scores is not None and len(m.scores) and len(m.book_time):
        p["score_touch"] = _reactions(m.scores[:, 0], m.book_time,
                                      np.column_stack([m.bid_price[:, 0], m.ask_price[:, 0]]))
        if m.value is not None and len(m.value):
            p["score_value"] = _reactions(m.scores[:, 0], m.value_time, m.value[:, None])

    # Calibration of a binary contract.
    if options.payout and m.outcome is not None:
        said = sampled[1:-1] / options.payout
        said = said[np.isfinite(said)]
        p["calibration"] = (said, np.full(len(said), m.outcome / options.payout))
    return p


def pieces(t: tapes.Tape, config: Config = Config()) -> list[dict]:
    return [market_pieces(m, t.options, config) for m in t.markets]


# -- pooling ---------------------------------------------------------------------------------

def _cat(ps: list[dict], key: str) -> np.ndarray:
    arrays = [p[key] for p in ps if key in p]
    return np.concatenate(arrays) if arrays else np.array([])


def _stack(ps: list[dict], key: str) -> np.ndarray | None:
    arrays = [p[key] for p in ps if key in p]
    return np.array(arrays) if arrays else None


def measure(ps: list[dict], config: Config = Config()) -> dict[str, float]:
    """The pooled statistics of a set of markets' pieces; NaN where a tape cannot give one."""
    out: dict[str, float] = {}
    spread, weight = _cat(ps, "spread"), _cat(ps, "spread_weight")
    out["spread_mean"] = float(spread @ weight / weight.sum()) if weight.sum() > 0 else np.nan
    out["spread_median"] = _weighted_quantile(spread, weight, 0.5)
    out["spread_one_tick"] = (float(weight[spread <= 1.5].sum() / weight.sum())
                              if weight.sum() > 0 else np.nan)
    sided = _stack(ps, "one_sided").sum(axis=0)
    out["one_sided"] = sided[0] / sided[1] if sided[1] > 0 else np.nan
    touch, touch_weight = _cat(ps, "touch"), _cat(ps, "touch_weight")
    out["touch_size_median"] = _weighted_quantile(touch, touch_weight, 0.5)
    out["touch_size_mean"] = (float(touch @ touch_weight / touch_weight.sum())
                              if touch_weight.sum() > 0 else np.nan)
    depth = _stack(ps, "depth").sum(axis=0)
    out["depth_mean"] = depth[0] / depth[1] if depth[1] > 0 else np.nan
    for ticks in (3, 10):
        near = _stack(ps, f"near_{ticks}").sum(axis=0)
        out[f"depth_near_{ticks}"] = near[0] / near[1] if near[1] > 0 else np.nan

    duration = sum(p["duration"] for p in ps)
    orders = _stack(ps, "orders").sum(axis=0)
    out["orders_per_minute"] = 60 * orders[0] / duration if duration > 0 else np.nan
    out["prints_per_order"] = orders[1] / orders[0] if orders[0] > 0 else np.nan
    sizes = _cat(ps, "sizes")
    median = float(np.median(sizes)) if len(sizes) else np.nan
    out["order_size_median"] = median
    out["order_size_tail"] = (float(np.quantile(sizes, 1 - config.tail)) / median
                              if len(sizes) and median > 0 else np.nan)
    out["order_size_hill"] = hill(sizes, config.tail)
    volume = sum(p["volume"] for p in ps)
    out["touch_turnover"] = (60 * volume / duration / out["touch_size_mean"]
                             if duration > 0 and out["touch_size_mean"] > 0 else np.nan)
    gap_values = _cat(ps, "gap_values")
    out["order_gap_median"] = float(np.median(gap_values)) if len(gap_values) else np.nan
    gaps = _stack(ps, "gaps").sum(axis=0)
    if gaps[0] > 2:
        mean = gaps[1] / gaps[0]
        variance = gaps[2] / gaps[0] - mean * mean
        out["order_gap_cv"] = math.sqrt(max(variance, 0.0)) / mean if mean > 0 else np.nan
    else:
        out["order_gap_cv"] = np.nan
    tenths = _stack(ps, "tenths").sum(axis=0)
    out["orders_first_tenth"] = tenths[0] / tenths[2] if tenths[2] else np.nan
    out["orders_last_tenth"] = tenths[1] / tenths[2] if tenths[2] else np.nan

    variances = {}
    for h in config.horizons:
        sd, kurtosis = _pooled_kurtosis(_stack(ps, f"returns_{h}"))
        out[f"return_sd_{h}"], out[f"return_kurtosis_{h}"] = sd, kurtosis
        variances[h] = sd * sd
    first = config.horizons[0]
    out[f"return_hill_{first}"] = hill(np.abs(_cat(ps, "returns_first")), config.tail)
    for h in config.horizons[1:]:
        out[f"variance_ratio_{h}"] = (variances[h] / (h / first * variances[first])
                                      if variances[first] > 0 else np.nan)
    out["return_autocorrelation"] = _pooled_autocorrelation(
        _stack(ps, "return_autocorrelation"), (1,))[0]
    for lag, value in zip(config.clustering_lags, _pooled_autocorrelation(
            _stack(ps, "clustering"), config.clustering_lags), strict=True):
        out[f"clustering_{lag}"] = value
    for lag, value in zip(config.sign_lags, _pooled_autocorrelation(
            _stack(ps, "signs"), config.sign_lags), strict=True):
        out[f"sign_memory_{lag}"] = value
    bounce = _pooled_autocorrelation(_stack(ps, "bounce"), (1,))[0]
    out["bounce"] = bounce
    # Roll (1984): successive changes have covariance −s²/4 for a spread s.
    sd, _ = _pooled_kurtosis(_stack(ps, "bounce_moments"))
    covariance = bounce * sd * sd if np.isfinite(bounce) else np.nan
    out["roll_spread"] = (2 * math.sqrt(-covariance)
                          if np.isfinite(covariance) and covariance < 0 else np.nan)

    names = ["at_trade"] + [f"after_{h:g}" for h in config.markouts] + ["settlement"]
    keys = ["maker_at_trade"] + [f"maker_after_{h:g}" for h in config.markouts] + [
        "maker_at_settlement"]
    for name, key in zip(names, keys, strict=True):
        rows = [p["maker"][name] for p in ps if name in p["maker"]]
        total = np.sum(rows, axis=0) if rows else np.zeros(2)
        out[key] = total[0] / total[1] if total[1] > 0 else np.nan

    follow = _stack(ps, "follow")
    if follow is not None:
        summed = follow.sum(axis=0)
        for lag, (cross, square) in zip(config.follow_lags, summed, strict=True):
            out[f"follow_{lag:g}"] = cross / square if square > 0 else np.nan
        lateness = _stack(ps, "lateness").sum(axis=0)
        with np.errstate(invalid="ignore", divide="ignore"):
            mean_gap = lateness[:, 0] / lateness[:, 1]
        out["delay"] = (float(np.nanargmin(mean_gap) * config.delay_step)
                        if np.any(np.isfinite(mean_gap)) else np.nan)
        out["value_distance"] = mean_gap[0]
    else:
        for lag in config.follow_lags:
            out[f"follow_{lag:g}"] = np.nan
        out["delay"] = out["value_distance"] = np.nan

    through = _stack(ps, "through")
    if through is not None and through.sum(axis=0)[2] > 0:
        total = through.sum(axis=0)
        out["touch_through_value"] = total[0] / total[2]
        out["touch_through_value_2"] = total[1] / total[2]
    else:
        out["touch_through_value"] = out["touch_through_value_2"] = np.nan
    for key, piece in (("score_touch", "score_touch"), ("score_value", "score_value")):
        delays = _cat(ps, piece)
        out[f"{key}_median"] = (float(np.nanmedian(delays))
                                if np.isfinite(delays).any() else np.nan)
        if key == "score_touch":
            out["score_touch_within_5"] = (float(np.mean(np.nan_to_num(delays, nan=np.inf) <= 5))
                                           if len(delays) else np.nan)

    said = [p["calibration"] for p in ps if "calibration" in p]
    if said:
        forecast = np.concatenate([s[0] for s in said])
        outcome = np.concatenate([s[1] for s in said])
        out["brier"] = float(np.mean((forecast - outcome) ** 2))
        for key, low, high in (("priced_low", 0.1, 0.2), ("priced_high", 0.8, 0.9)):
            chosen = (forecast >= low) & (forecast < high)
            out[key] = float(outcome[chosen].mean()) if chosen.sum() >= 20 else np.nan
    else:
        out["brier"] = out["priced_low"] = out["priced_high"] = np.nan
    return out


def bootstrap(ps: list[dict], config: Config = Config(), seed: int = 1) -> dict[str, np.ndarray]:
    """Each statistic over `config.bootstrap` resamples of the markets, with replacement."""
    rng = np.random.default_rng(seed)
    draws = []
    for _ in range(config.bootstrap):
        chosen = rng.integers(0, len(ps), len(ps))
        draws.append(measure([ps[i] for i in chosen], config))
    return {key: np.array([d[key] for d in draws]) for key in draws[0]}


@dataclass
class Summary:
    """A tape's statistics with their resamples, and the samples its distributions are compared
    on."""

    name: str
    markets: int
    point: dict[str, float]
    draws: dict[str, np.ndarray]
    pieces: list[dict]


def summarize(name: str, t: tapes.Tape, config: Config = Config(), seed: int = 1) -> Summary:
    ps = pieces(t, config)
    return Summary(name, len(ps), measure(ps, config), bootstrap(ps, config, seed), ps)


def _scaled(values, logarithmic: bool):
    values = np.asarray(values, dtype=float)
    if not logarithmic:
        return values
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(values > 0, np.log(np.where(values > 0, values, 1.0)), np.nan)


def spread_of(draws: np.ndarray, logarithmic: bool) -> float:
    scaled = _scaled(draws, logarithmic)
    scaled = scaled[np.isfinite(scaled)]
    return float(np.std(scaled)) if len(scaled) > 2 else np.nan


def distance(real: Summary, simulated: Summary, key: str, logarithmic: bool) -> float:
    """How far the simulated statistic is from the real one, over the two resampling spreads
    together."""
    a = _scaled(real.point[key], logarithmic)
    b = _scaled(simulated.point[key], logarithmic)
    tolerance = math.hypot(spread_of(real.draws[key], logarithmic),
                           spread_of(simulated.draws[key], logarithmic))
    if not (np.isfinite(a) and np.isfinite(b) and np.isfinite(tolerance)) or tolerance <= 0:
        return np.nan
    return float((b - a) / tolerance)


def ks(a: np.ndarray, b: np.ndarray) -> float:
    """The Kolmogorov–Smirnov distance between two samples."""
    a, b = np.sort(a[np.isfinite(a)]), np.sort(b[np.isfinite(b)])
    if len(a) == 0 or len(b) == 0:
        return np.nan
    grid = np.concatenate([a, b])
    return float(np.max(np.abs(np.searchsorted(a, grid, side="right") / len(a)
                               - np.searchsorted(b, grid, side="right") / len(b))))


# A statistic passes when its distance is within this many tolerances: about 95% of honest ones.
PASS = 2.0

DISTRIBUTIONS = [
    ("spread_grid", "spread (ticks)"),
    ("touch_grid", "size at the touch"),
    ("sizes", "order size"),
    ("returns_first", "one-step mid change"),
]


def distributions(real: Summary, simulated: Summary, seed: int = 1, splits: int = 20) -> list:
    """For each distribution: the KS distance between real and simulated, and the median distance
    between two random halves of the real markets."""
    rng = np.random.default_rng(seed)
    rows = []
    for key, label in DISTRIBUTIONS:
        found = ks(_cat(real.pieces, key), _cat(simulated.pieces, key))
        chance = []
        if real.markets >= 4:
            for _ in range(splits):
                order = rng.permutation(real.markets)
                half = real.markets // 2
                chance.append(ks(_cat([real.pieces[i] for i in order[:half]], key),
                                 _cat([real.pieces[i] for i in order[half:]], key)))
        rows.append((label, found, float(np.nanmedian(chance)) if chance else np.nan))
    return rows


def _format(value: float) -> str:
    if not np.isfinite(value):
        return "–"
    magnitude = abs(value)
    if magnitude >= 1000:
        return f"{value:,.0f}"
    if magnitude >= 10:
        return f"{value:.1f}"
    return f"{value:.3g}"


def _interval(draws: np.ndarray) -> str:
    finite = draws[np.isfinite(draws)]
    if len(finite) < 3:
        return ""
    low, high = np.percentile(finite, [5, 95])
    return f" [{_format(low)}, {_format(high)}]"


def report(real: Summary, simulated: list[Summary], config: Config = Config()) -> tuple[str, dict]:
    """The report as Markdown, and the same numbers as a dictionary for JSON."""
    lines = [f"# Validation: {', '.join(s.name for s in simulated)} against {real.name}", ""]
    lines.append(f"{real.name}: {real.markets} markets; " + "; ".join(
        f"{s.name}: {s.markets}" for s in simulated) + ". Intervals are the 5th to 95th "
        f"percentile over {config.bootstrap} resamples of markets; a distance is the difference "
        "in those tolerances, logarithmic where marked (log), and a statistic passes within "
        f"{PASS:g} of them.")
    lines.append("")
    header = f"| statistic | {real.name} |"
    rule = "|---|---:|"
    for s in simulated:
        header += f" {s.name} | distance | pass |"
        rule += "---:|---:|:---:|"
    lines += [header, rule]
    data: dict = {"real": {"name": real.name, "markets": real.markets},
                  "simulated": [{"name": s.name, "markets": s.markets} for s in simulated],
                  "statistics": []}
    totals = [[] for _ in simulated]
    for key, label, logarithmic in statistic_names(config):
        values = [real.point.get(key, np.nan)] + [s.point.get(key, np.nan) for s in simulated]
        if not any(np.isfinite(v) for v in values):
            continue
        row = f"| {label}{' (log)' if logarithmic else ''} | {_format(real.point[key])}" \
              f"{_interval(real.draws[key])} |"
        entry = {"key": key, "label": label, "logarithmic": logarithmic,
                 "real": real.point[key], "real_interval": list(np.nanpercentile(
                     real.draws[key], [5, 95])) if np.any(np.isfinite(real.draws[key])) else None,
                 "simulated": []}
        for i, s in enumerate(simulated):
            gap = distance(real, s, key, logarithmic)
            if np.isfinite(gap):
                totals[i].append(gap)
            passed = None if not np.isfinite(gap) else bool(abs(gap) <= PASS)
            row += f" {_format(s.point[key])}{_interval(s.draws[key])} | " \
                   f"{'' if not np.isfinite(gap) else f'{gap:+.1f}'} | " \
                   f"{'' if passed is None else ('yes' if passed else 'no')} |"
            entry["simulated"].append({"value": s.point[key], "distance": gap, "pass": passed})
        lines.append(row)
        data["statistics"].append(entry)
    summary = "| **passed** |  |"
    for gaps in totals:
        inside = sum(abs(g) <= PASS for g in gaps)
        summary += f" | | **{inside} of {len(gaps)}** |"
    lines.append(summary)
    score = "| **distance, sum of squares** |  |"
    for gaps in totals:
        score += f" | **{sum(g * g for g in gaps):,.0f}** | |"
    lines.append(score)
    data["summary"] = [{"passed": int(sum(abs(g) <= PASS for g in gaps)),
                        "compared": len(gaps), "distance": float(sum(g * g for g in gaps))}
                       for gaps in totals]
    lines += ["", "## Distributions", "",
              "Kolmogorov–Smirnov distance, pooled over markets; chance is the median distance "
              "between two random halves of the real markets.", ""]
    lines.append("| distribution | " + " | ".join(s.name for s in simulated) + " | chance |")
    lines.append("|---|" + "---:|" * (len(simulated) + 1))
    data["distributions"] = []
    for index, (key, label) in enumerate(DISTRIBUTIONS):
        cells, chance = [], np.nan
        for s in simulated:
            _, found, chance = distributions(real, s)[index]
            cells.append(_format(found))
        lines.append(f"| {label} | " + " | ".join(cells) + f" | {_format(chance)} |")
        data["distributions"].append({"key": key, "label": label, "chance": chance,
                                      "simulated": cells})
    return "\n".join(lines) + "\n", data


def _ccdf(x: np.ndarray, points: int = 60) -> tuple[np.ndarray, np.ndarray]:
    """The share of samples above each of `points` levels spaced in proportion."""
    x = np.sort(x[np.isfinite(x) & (x > 0)])
    if len(x) < 10:
        return np.array([]), np.array([])
    levels = np.geomspace(x[0], x[-1], points)
    above = 1 - np.searchsorted(x, levels, side="right") / len(x)
    keep = above > 0
    return levels[keep], above[keep]


def plot(real: Summary, simulated: list[Summary], config: Config, directory: Path) -> list[Path]:
    """Charts of the report, real against each synthetic market: the spread's distribution, the
    tails of order sizes and of one-step changes, how resting orders' earnings move with the
    horizon, and, where there is a value, how fast the mid follows it. Returns the files."""
    from crowdbook_analysis import style

    summaries = [real] + simulated
    made = []

    def save(fig, ax, name: str) -> None:
        path = directory / f"{name}.png"
        style.finish(fig, ax, path)
        made.append(path)

    fig, ax = style.figure("Spread at the touch", "ticks", "share of samples")
    for slot, s in enumerate(summaries[:4]):
        spreads = np.round(_cat(s.pieces, "spread_grid"))
        spreads = spreads[np.isfinite(spreads)]
        values = np.arange(1, 9)
        share = [np.mean(spreads == v) if len(spreads) else 0 for v in values]
        style.stems(ax, values, share, slot, s.name,
                    offset=(slot - 0.5 * (len(summaries) - 1)) * 0.12)
    save(fig, ax, "spread")

    for key, title, xlabel, name in (("sizes", "Order sizes: the tail", "order size", "sizes"),
                                     ("returns_first", "One-step mid changes: the tail",
                                      "size of change", "tails")):
        fig, ax = style.figure(title, xlabel, "share larger")
        for slot, s in enumerate(summaries[:4]):
            x, y = _ccdf(np.abs(_cat(s.pieces, key)))
            if len(x):
                style.line(ax, x, y, slot, s.name, markers=False)
        ax.set_xscale("log")
        ax.set_yscale("log")
        save(fig, ax, name)

    horizons = [0.0] + list(config.markouts)
    keys = ["maker_at_trade"] + [f"maker_after_{h:g}" for h in config.markouts]
    fig, ax = style.figure("What resting orders earn, by horizon", "seconds after the fill",
                           "earned per unit (ticks)")
    for slot, s in enumerate(summaries[:4]):
        values = [s.point.get(k, np.nan) for k in keys]
        errors = [spread_of(s.draws[k], False) for k in keys]
        style.line(ax, horizons, values, slot, s.name, error=errors)
    ax.set_xscale("symlog", linthresh=1)
    ax.set_xticks(horizons, [f"{h:g}" for h in horizons])
    save(fig, ax, "markouts")

    if any(np.isfinite(s.point.get(f"follow_{config.follow_lags[0]:g}", np.nan))
           for s in summaries):
        keys = [f"follow_{lag:g}" for lag in config.follow_lags]
        fig, ax = style.figure(f"How the mid follows a {config.move:g} s move of the value",
                               "seconds after the move", "share of the move made")
        for slot, s in enumerate(summaries[:4]):
            values = [s.point.get(k, np.nan) for k in keys]
            if np.any(np.isfinite(values)):
                errors = [spread_of(s.draws[k], False) for k in keys]
                style.line(ax, list(config.follow_lags), values, slot, s.name, error=errors)
        save(fig, ax, "follow")
    return made


def _seeds(text: str) -> range:
    first, _, last = text.partition(":")
    return range(int(first), int(last)) if last else range(1, int(first) + 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--real", type=Path, required=True, help="the real market's tape")
    parser.add_argument("--sim", type=Path, action="append", default=[],
                        help="a synthetic market's tape (repeat for several)")
    parser.add_argument("--scenario", type=Path, action="append", default=[],
                        help="a scenario to run into a tape first (repeat for several)")
    parser.add_argument("--seeds", default="100001:100401",
                        help="seeds for --scenario, FIRST:END or a count (default 100001:100401)")
    parser.add_argument("--grid", type=float, default=Config.grid,
                        help="seconds between samples of the mid (default 1)")
    parser.add_argument("--bootstrap", type=int, default=Config.bootstrap)
    parser.add_argument("--window", help="measure only from START to END seconds of each market, "
                        "START:END, or START: to its end")
    parser.add_argument("--phases", metavar="LABEL",
                        help="report the part of each market before the time a label gives, such "
                        "as kickoff, and the part from it, apart")
    parser.add_argument("--crowdbook", help="the crowdbook executable")
    parser.add_argument("--out", type=Path, help="write the Markdown here, not to standard output")
    parser.add_argument("--json", type=Path, help="also write the numbers as JSON")
    parser.add_argument("--plots", type=Path, help="also draw the charts into this directory")
    args = parser.parse_args()
    if not args.sim and not args.scenario:
        parser.error("give at least one --sim or --scenario")
    config = Config(grid=args.grid, bootstrap=args.bootstrap)

    def cut(t: tapes.Tape) -> tapes.Tape:
        if not args.window:
            return t
        start, _, end = args.window.partition(":")
        return tapes.window(t, float(start or 0), float(end) if end else None)

    real_tape = cut(tapes.read_tape(args.real))
    named = [(path.name, cut(tapes.read_tape(path))) for path in args.sim]
    if args.scenario:
        tool = find_tool(args.crowdbook)
        for path in args.scenario:
            named.append((path.stem, cut(tapes.simulate(tool, path.read_text(),
                                                        _seeds(args.seeds)))))
    parts = [("", lambda t: t)]
    if args.phases:
        parts = [(f"before {args.phases}", lambda t: tapes.phase(t, args.phases, False)),
                 (f"from {args.phases}", lambda t: tapes.phase(t, args.phases, True))]
    texts, everything = [], {}
    for title, part in parts:
        real = summarize(args.real.name, part(real_tape), config, seed=1)
        simulated = [summarize(name, part(t), config, seed=2 + i)
                     for i, (name, t) in enumerate(named)]
        text, data = report(real, simulated, config)
        if title:
            text = text.replace("# Validation:", f"# Validation, {title}:", 1)
        texts.append(text)
        everything[title or "whole"] = data
        if args.plots:
            plot(real, simulated[:3], config,
                 args.plots / title.replace(" ", "_") if title else args.plots)
    text = "\n".join(texts)
    if args.out:
        args.out.write_text(text)
    else:
        print(text)
    if args.json:
        args.json.write_text(json.dumps(everything if args.phases else everything["whole"],
                                        indent=1, default=float))


if __name__ == "__main__":
    main()
