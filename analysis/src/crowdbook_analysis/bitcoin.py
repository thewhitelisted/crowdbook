"""Bitcoin's price each second, and from it the probability that Bitcoin ends a window higher
than it began: a stand-in for the true value of an up-or-down question, so that real prices can
be compared with a value as simulated ones are.

The prices are Binance's one-second candles for BTCUSDT, published as one file a day at
data.binance.vision; Polymarket's hourly questions resolve on the same candles, and its shorter
ones on Chainlink's BTC/USD price, which follows the same trading. The probability treats the
price as a random walk without drift over what is left of the window, with the volatility of the
hour before: P(up) = Φ(ln(S/S₀) / (σ√τ)), for the price S now and S₀ at the window's start, the
volatility σ per square root of a second, and τ seconds left.
"""

import datetime as dt
import io
import math
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

import numpy as np

from crowdbook_analysis.polygon import USER_AGENT
from crowdbook_analysis.runner import REPO

DATA = REPO / "analysis" / "data" / "binance"
SOURCE = "https://data.binance.vision/data/spot/daily/klines/BTCUSDT/1s/BTCUSDT-1s-{day}.zip"
VOLATILITY_WINDOW = 3600  # seconds of history the volatility is measured over


def fetch(day: dt.date, data: Path = DATA) -> Path | None:
    """The day's closing prices each second, as a NumPy file of (second, price) rows, downloaded
    once; None for a day not published yet."""
    path = data / f"{day.isoformat()}.npy"
    if not path.exists():
        request = urllib.request.Request(SOURCE.format(day=day.isoformat()),
                                         headers={"user-agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                archive = zipfile.ZipFile(io.BytesIO(response.read()))
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise
        rows = np.loadtxt(archive.open(archive.namelist()[0]), delimiter=",", usecols=(0, 4))
        # Open times are in microseconds since 2025, in milliseconds before.
        scale = 1_000_000 if rows[0, 0] > 1e14 else 1_000
        data.mkdir(parents=True, exist_ok=True)
        np.save(path, np.column_stack([rows[:, 0] // scale, rows[:, 1]]))
    return path


class Prices:
    """Bitcoin's price at any whole second over a run of days, carried forward over seconds
    without a candle."""

    def __init__(self, first: dt.date, last: dt.date, data: Path = DATA):
        days = [first + dt.timedelta(days=n) for n in range((last - first).days + 1)]
        paths = [path for path in (fetch(day, data) for day in days) if path is not None]
        rows = np.concatenate([np.load(path) for path in paths])
        self.start = int(rows[0, 0])
        seconds = np.arange(self.start, int(rows[-1, 0]) + 1)
        index = np.searchsorted(rows[:, 0], seconds, side="right") - 1
        self.price = rows[index, 1]
        # A missing day is a gap, not a price carried through it.
        stale = seconds - rows[index, 0] > 60
        self.price[stale] = np.nan

    def at(self, seconds: np.ndarray) -> np.ndarray:
        index = np.asarray(seconds, dtype=np.int64) - self.start
        inside = (index >= 0) & (index < len(self.price))
        out = np.full(index.shape, np.nan)
        out[inside] = self.price[index[inside]]
        return out


def _normal_cdf(x: np.ndarray) -> np.ndarray:
    return 0.5 * (1.0 + np.vectorize(math.erf)(x / math.sqrt(2.0)))


def up_probability(prices: Prices, start: int, end: int) -> np.ndarray:
    """The probability, in cents, that Bitcoin ends the window from `start` to `end` (Unix
    seconds) above where it began, at each second of the window; NaN where prices are missing."""
    before = prices.at(np.arange(start - VOLATILITY_WINDOW, start + 1))
    returns = np.diff(np.log(before))
    seconds = np.arange(start, end)
    if np.isnan(returns).all():
        return np.full(len(seconds), np.nan)
    sigma = float(np.nanstd(returns))
    now = prices.at(seconds)
    left = (end - seconds).astype(float)
    with np.errstate(divide="ignore", invalid="ignore"):
        z = np.log(now / prices.at(np.array([start]))[0]) / (sigma * np.sqrt(left))
    return 100.0 * _normal_cdf(z)
