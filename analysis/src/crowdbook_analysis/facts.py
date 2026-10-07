"""Statistics of simulated markets, compared in the stylized-facts literature with real ones."""

import numpy as np
import polars as pl


def sample_last(times: np.ndarray, values: np.ndarray, grid: np.ndarray) -> np.ndarray:
    """The value in effect at each grid time: the last one at or before it, or NaN before any."""
    if len(times) == 0:
        return np.full(len(grid), np.nan)
    index = np.searchsorted(times, grid, side="right") - 1
    sampled = values[np.clip(index, 0, None)].astype(float)
    sampled[index < 0] = np.nan
    return sampled


def log_returns(prices: np.ndarray) -> np.ndarray:
    prices = prices[~np.isnan(prices)]
    return np.diff(np.log(prices))


def _windows(mids: np.ndarray, steps: int) -> np.ndarray:
    """Every `steps`-th sample of a regularly sampled mid, from the first with a mid. Gaps stay
    in, as NaN, so that every window spans exactly `steps` samples."""
    quoted = np.flatnonzero(~np.isnan(mids))
    return mids[quoted[0] :: steps] if len(quoted) else mids[:0]


def horizon_returns(mids: np.ndarray, steps: int) -> np.ndarray:
    """Log returns over non-overlapping windows of `steps` samples of a regularly sampled mid,
    leaving out windows that start or end without a mid."""
    returns = np.diff(np.log(_windows(mids, steps)))
    return returns[~np.isnan(returns)]


def volatility_per_root_second(mids: np.ndarray, steps: int, step_seconds: float) -> float:
    """The standard deviation of mid changes over `steps` samples, in ticks, scaled to one
    second by the square root of time, leaving out changes that start or end without a mid. A
    random walk gives the same value at every horizon; microstructure noise makes it larger at
    short horizons."""
    changes = np.diff(_windows(mids, steps))
    return float(changes[~np.isnan(changes)].std() / np.sqrt(steps * step_seconds))


def markouts(
    executions: pl.DataFrame, quotes: pl.DataFrame, horizons_ns: list[int], end_ns: int
) -> pl.DataFrame:
    """For each execution and horizon, how far the mid has moved in the maker's favour from the
    execution's price, in ticks per lot: what providing that liquidity earned once the price had
    moved on. Executions too close to `end_ns` to see a horizon are left out of it."""
    both = quotes.drop_nulls("mid")
    times = both["time"].to_numpy()
    mids = both["mid"].to_numpy()
    traded = executions["time"].to_numpy()
    side = np.where(executions["maker_side"].to_numpy() == "buy", 1.0, -1.0)
    price = executions["price"].to_numpy().astype(float)
    frames = []
    for horizon in horizons_ns:
        mid = sample_last(times, mids, traded + horizon)
        frames.append(
            executions.with_columns(
                horizon_ns=pl.lit(horizon, dtype=pl.Int64),
                markout=pl.Series(side * (mid - price)),
                seen=pl.Series(traded + horizon <= end_ns),
            )
            .filter(pl.col("seen") & pl.col("markout").is_not_nan())
            .drop("seen")
        )
    return pl.concat(frames)


def mean_and_error(values) -> tuple:
    """The mean over runs (the first axis) and its standard error, leaving out NaN."""
    array = np.asarray(values, dtype=float)
    runs = np.sum(~np.isnan(array), axis=0)
    return np.nanmean(array, axis=0), np.nanstd(array, axis=0, ddof=1) / np.sqrt(runs)


def excess_kurtosis(x: np.ndarray) -> float:
    """Zero for a normal distribution; positive for fat tails."""
    centered = x - x.mean()
    variance = np.mean(centered**2)
    return float(np.mean(centered**4) / variance**2 - 3.0)


def autocorrelation(x: np.ndarray, max_lag: int) -> np.ndarray:
    """The sample autocorrelation at lags 1 to `max_lag`."""
    centered = x - x.mean()
    variance = np.dot(centered, centered)
    return np.array(
        [np.dot(centered[:-lag], centered[lag:]) / variance for lag in range(1, max_lag + 1)]
    )


def tail_distribution(x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """For returns standardized to unit variance: each |return|, sorted, with the share of
    returns at least that large (the complementary cumulative distribution)."""
    standardized = np.sort(np.abs((x - x.mean()) / x.std()))
    share_at_least = 1.0 - np.arange(len(standardized)) / len(standardized)
    return standardized, share_at_least


def normal_tail(z: np.ndarray) -> np.ndarray:
    """P(|Z| >= z) for a standard normal Z."""
    from math import erfc, sqrt

    return np.array([erfc(value / sqrt(2.0)) for value in z])


def spread_shares(quotes: pl.DataFrame, end_ns: int) -> dict[int, float]:
    """The share of time the spread spent at each width, in ticks, while both sides were quoted.
    Each quote lasts until the next change."""
    durations = quotes.with_columns(
        lasted=pl.col("time").shift(-1).fill_null(end_ns) - pl.col("time")
    ).drop_nulls("spread")
    totals = durations.group_by("spread").agg(pl.col("lasted").sum()).sort("spread")
    whole = totals["lasted"].sum()
    return {
        int(spread): float(lasted) / whole
        for spread, lasted in zip(totals["spread"], totals["lasted"], strict=True)
    }


def impact_response(
    orders: pl.DataFrame, quotes: pl.DataFrame, horizons_ns: list[int], by: str, end_ns: int
) -> pl.DataFrame:
    """How far the mid moves in each aggressive order's direction, in ticks, from just before the
    order traded to each horizon after: the mean and its standard error over the orders of each
    `by` group. Orders too close to `end_ns` to see a horizon are left out of it. The standard
    error treats orders as independent, which understates it once a horizon is long enough for
    many orders to share one price path; for those, compare independent runs instead."""
    both = quotes.drop_nulls("mid")
    times = both["time"].to_numpy()
    mids = both["mid"].to_numpy()
    order_times = orders["time"].to_numpy()
    before = sample_last(times, mids, order_times - 1)
    direction = np.where(orders["side"].to_numpy() == "buy", 1.0, -1.0)
    moves = []
    for horizon in horizons_ns:
        after = sample_last(times, mids, order_times + horizon)
        moves.append(
            orders.select(by)
            .with_columns(
                horizon_ns=pl.lit(horizon, dtype=pl.Int64),
                move=pl.Series((after - before) * direction),
                seen=pl.Series(order_times + horizon <= end_ns),
            )
            .filter(pl.col("seen") & pl.col("move").is_not_nan())
        )
    return (
        pl.concat(moves)
        .group_by(by, "horizon_ns")
        .agg(
            pl.col("move").mean(),
            (pl.col("move").std() / pl.len().sqrt()).alias("error"),
            pl.len().alias("orders"),
        )
        .sort(by, "horizon_ns")
    )
