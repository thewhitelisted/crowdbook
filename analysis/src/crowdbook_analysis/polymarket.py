"""Polymarket's trades and outcomes, read from its public blockchain, for comparing real
prediction markets with simulated ones.

    uv run --project analysis crowdbook-polymarket --from 2026-09-29 --to 2026-10-12

Every Polymarket trade settles on Polygon, so its record is public. Each one is an `OrderFilled`
event of Polymarket's exchange contract; this reads the fills of the order that took liquidity,
one per aggressive order, with its side, shares and price. How each question resolved comes from
the `ConditionResolution` event of the Conditional Tokens contract that holds the shares. Which
question is which comes from Polymarket's public list of markets: its slug, the condition id of
its shares and the window it asks about. The share ids are worked out again from the condition id
as the Conditional Tokens contract does, so the list is checked against the chain, not trusted.

The markets read are short recurring questions, such as whether Bitcoin will be up or down at the
end of the next fifteen minutes: hundreds resolve each day, each one a question that opens at a
known time with nothing yet known and resolves at a known time, the shape of a simulated
prediction market.

Fills are kept for the questions in the list only, one file per day and series, so a day already
read is not read again.
"""

import argparse
import datetime as dt
import json
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import polars as pl

from crowdbook_analysis.polygon import (
    DEFAULT_RPCS,
    USER_AGENT,
    Rpc,
    address_topic,
    event_topic,
    keccak256,
    words,
)
from crowdbook_analysis.runner import REPO

# The second version of Polymarket's exchange, which settles markets that are not part of a group
# of questions, such as the up-or-down ones; and the Conditional Tokens contract that holds the
# shares, backed by bridged USDC. Markets that are part of a group settle on another exchange,
# not read here.
EXCHANGE = "0xe111180000d2663c0091e4f400237545b87b996b"
CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045"
COLLATERAL = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174"

ORDER_FILLED = event_topic(
    "OrderFilled(bytes32,address,address,uint8,uint256,uint256,uint256,uint256,bytes32,bytes32)")
CONDITION_RESOLUTION = event_topic("ConditionResolution(bytes32,address,bytes32,uint256,uint256[])")

MARKETS_API = "https://gamma-api.polymarket.com"
DATA = REPO / "analysis" / "data" / "polymarket"
SERIES = ("btc-up-or-down-5m", "btc-up-or-down-15m", "btc-up-or-down-hourly")
UNITS = 1_000_000  # shares and dollars both have six decimals on chain

# The base field of the alt_bn128 curve, on which the Conditional Tokens contract maps a
# condition and outcome to a point to form a share's id.
_FIELD = 0x30644E72E131A029B85045B68181585D97816A916871CA8D3C208C16D87CFD47


def _collection_id(condition: bytes, index_set: int) -> int:
    """The Conditional Tokens contract's id for a set of a condition's outcomes, with no parent
    collection: a hash of the two lifted to a point of the curve y² = x³ + 3, compressed."""
    x = int.from_bytes(keccak256(condition + index_set.to_bytes(32, "big")), "big")
    odd = x >> 255 != 0
    while True:
        x = (x + 1) % _FIELD
        square = (x * x * x + 3) % _FIELD
        y = pow(square, (_FIELD + 1) // 4, _FIELD)  # a square root, since the field is 3 mod 4
        if y * y % _FIELD == square:
            break
    if odd == (y % 2 == 0):
        y = _FIELD - y
    if y % 2 == 1:
        x ^= 1 << 254
    return x


def share_id(condition: str, outcome: int) -> int:
    """The id of the shares that pay if a condition resolves to `outcome`, counted from 0."""
    collection = _collection_id(bytes.fromhex(condition.removeprefix("0x")), 1 << outcome)
    return int.from_bytes(keccak256(bytes.fromhex(COLLATERAL.removeprefix("0x"))
                                    + collection.to_bytes(32, "big")), "big")


@dataclass(frozen=True)
class Market:
    """A yes-or-no question and the window it asks about, in Unix seconds. `yes` is the share
    of the first outcome, such as Up, and `no` of the second."""

    series: str
    slug: str
    condition: str
    start: int
    end: int
    yes: int
    no: int


def _get(url: str):
    request = urllib.request.Request(url, headers={"user-agent": USER_AGENT})
    for attempt in range(6):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code in (429, 500, 502, 503, 504) and attempt < 5:
                time.sleep(2.0**attempt)
                continue
            raise
    raise AssertionError("unreachable")


def _seconds(text: str) -> int:
    return int(dt.datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp())


def list_markets(series: str, ending_from: int, ending_to: int) -> list[Market]:
    """The closed markets of a recurring series, such as `btc-up-or-down-15m`, whose windows end
    between two times, in Unix seconds. Markets whose share ids do not match their condition are
    left out, and so are those without a window."""
    found = _get(f"{MARKETS_API}/series?slug={urllib.parse.quote(series)}")
    if not found:
        raise SystemExit(f"no series called {series}")
    series_id = found[0]["id"]
    markets, offset = [], 0
    while True:
        # Pages come in a fixed order, so that offsets do not skip or repeat any; the list
        # sends at most a hundred at a time, whatever the limit asked.
        query = urllib.parse.urlencode({
            "series_id": series_id, "closed": "true", "limit": 100, "offset": offset,
            "order": "endDate", "ascending": "true",
            "end_date_min": dt.datetime.fromtimestamp(ending_from, dt.UTC).isoformat(),
            "end_date_max": dt.datetime.fromtimestamp(ending_to, dt.UTC).isoformat()})
        events = _get(f"{MARKETS_API}/events?{query}")
        for event in events:
            for market in event.get("markets", []):
                if not market.get("eventStartTime") or not market.get("clobTokenIds"):
                    continue
                shares = [int(token) for token in json.loads(market["clobTokenIds"])]
                condition = market["conditionId"].lower()
                if len(shares) != 2 or shares != [share_id(condition, 0), share_id(condition, 1)]:
                    continue
                markets.append(Market(series, market["slug"], condition,
                                      _seconds(market["eventStartTime"]),
                                      _seconds(market["endDate"]), shares[0], shares[1]))
        if not events:
            break
        offset += len(events)
    return sorted(set(markets), key=lambda market: (market.start, market.slug))


def taker_fill(log: dict) -> dict:
    """An aggressive order's fill, from the exchange's `OrderFilled` event for the order that took
    liquidity: who sent it, the share it traded, which way, how many shares, at what price in
    cents, and the fee in dollars."""
    side, token, maker_amount, taker_amount, fee = words(log["data"])[:5]
    buy = side == 0
    shares, dollars = (taker_amount, maker_amount) if buy else (maker_amount, taker_amount)
    return {"block": int(log["blockNumber"], 16), "index": int(log["logIndex"], 16),
            "time": int(log["blockTimestamp"], 16), "trader": "0x" + log["topics"][2][-40:],
            "token": token, "buy": buy, "shares": shares / UNITS,
            "price": 100.0 * dollars / shares if shares else float("nan"), "fee": fee / UNITS}


def resolution(log: dict) -> dict:
    """A condition's resolution: the share of the payout each outcome gets, from the Conditional
    Tokens contract's `ConditionResolution` event."""
    values = words(log["data"])
    count = values[2]
    payouts = values[3 : 3 + count]
    total = sum(payouts)
    return {"condition": log["topics"][1].lower(), "time": int(log["blockTimestamp"], 16),
            "yes_payout": payouts[0] / total if total else float("nan")}


TRADES = {"series": pl.Utf8, "slug": pl.Utf8, "block": pl.Int64, "index": pl.Int64,
          "time": pl.Int64, "trader": pl.Utf8, "side": pl.Utf8, "price": pl.Float64,
          "shares": pl.Float64, "fee": pl.Float64, "on_yes": pl.Boolean}
OUTCOMES = {"series": pl.Utf8, "slug": pl.Utf8, "time": pl.Int64, "yes_payout": pl.Float64}
MARKETS = {"series": pl.Utf8, "slug": pl.Utf8, "condition": pl.Utf8, "start": pl.Int64,
           "end": pl.Int64, "yes": pl.Utf8, "no": pl.Utf8}


def read_day(rpc: Rpc, day: dt.date, markets: list[Market]) -> tuple[pl.DataFrame, pl.DataFrame]:
    """Every aggressive order in the given markets during one UTC day, as seen by the yes share:
    buying no is selling yes at 100 less the price. And every resolution of their conditions that
    day."""
    begin = int(dt.datetime.combine(day, dt.time(), dt.UTC).timestamp())
    first, last = rpc.block_at(begin), rpc.block_at(begin + 86_400) - 1
    by_token = {}
    for market in markets:
        by_token[market.yes] = (market, True)
        by_token[market.no] = (market, False)
    rows = []
    for log in rpc.logs(EXCHANGE, [ORDER_FILLED, None, None, address_topic(EXCHANGE)],
                        first, last):
        fill = taker_fill(log)
        found = by_token.get(fill.pop("token"))
        if found is None:
            continue
        market, on_yes = found
        buy = fill.pop("buy")
        rows.append(fill | {"series": market.series, "slug": market.slug, "on_yes": on_yes,
                            "side": "buy" if buy == on_yes else "sell",
                            "price": fill["price"] if on_yes else 100.0 - fill["price"]})
    by_condition = {market.condition: market for market in markets}
    outcomes = []
    for log in rpc.logs(CONDITIONAL_TOKENS, [CONDITION_RESOLUTION], first, last):
        settled = resolution(log)
        market = by_condition.get(settled["condition"])
        if market is not None:
            outcomes.append({"series": market.series, "slug": market.slug,
                             "time": settled["time"], "yes_payout": settled["yes_payout"]})
    trades = pl.DataFrame(rows, schema=TRADES).select(list(TRADES)).sort("block", "index")
    return trades, pl.DataFrame(outcomes, schema=OUTCOMES)


def _market_frame(markets: list[Market]) -> pl.DataFrame:
    return pl.DataFrame([{"series": m.series, "slug": m.slug, "condition": m.condition,
                          "start": m.start, "end": m.end, "yes": str(m.yes), "no": str(m.no)}
                         for m in markets], schema=MARKETS)


def fetch(rpc: Rpc, series: list[str], first: dt.date, last: dt.date, data: Path) -> None:
    """Reads every day from `first` to `last` that is not already in `data`: for each series,
    the markets whose windows start that day, and the day's trades and resolutions in every
    market of the series open then: those whose windows start a day or two later, and those that
    ended the day before, which may resolve after midnight."""
    day = first
    while day <= last:
        folder = data / day.isoformat()
        missing = [name for name in series if not (folder / f"{name}.trades.parquet").exists()]
        if missing:
            begin = int(dt.datetime.combine(day, dt.time(), dt.UTC).timestamp())
            markets = []
            for name in missing:
                markets += list_markets(name, begin - 86_400, begin + 3 * 86_400)
            started = time.monotonic()
            trades, outcomes = read_day(rpc, day, markets)
            folder.mkdir(parents=True, exist_ok=True)
            for name in missing:
                own = [m for m in markets if m.series == name and begin <= m.start < begin + 86_400]
                _market_frame(own).write_parquet(folder / f"{name}.markets.parquet")
                outcomes.filter(pl.col("series") == name).write_parquet(
                    folder / f"{name}.outcomes.parquet")
                # Written last: its presence marks the day as read.
                trades.filter(pl.col("series") == name).write_parquet(
                    folder / f"{name}.trades.parquet")
            print(f"{day}: {len(trades)} trades, {len(outcomes)} resolutions in "
                  f"{time.monotonic() - started:.0f}s", flush=True)
        day += dt.timedelta(days=1)


def load(series: str, data: Path = DATA) -> tuple[pl.DataFrame, pl.DataFrame]:
    """Every day read of one series: its markets, with how each resolved and when, and its
    trades, in the order they happened. Only markets whose resolution was read are kept."""
    def every(kind: str, schema: dict) -> pl.DataFrame:
        paths = sorted(data.glob(f"*/{series}.{kind}.parquet"))
        if not paths:
            return pl.DataFrame(schema=schema)
        return pl.concat([pl.read_parquet(path) for path in paths])

    outcomes = every("outcomes", OUTCOMES).group_by("slug").agg(
        pl.col("yes_payout").first(), pl.col("time").min().alias("resolved"))
    markets = every("markets", MARKETS).unique("slug").join(outcomes, on="slug").sort("start")
    trades = every("trades", TRADES).unique(["block", "index"]).sort("block", "index")
    return markets, trades.filter(pl.col("slug").is_in(markets["slug"].implode()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--from", dest="first", type=dt.date.fromisoformat, required=True,
                        help="the first UTC day to read, such as 2026-09-29")
    parser.add_argument("--to", dest="last", type=dt.date.fromisoformat, required=True,
                        help="the last UTC day to read")
    parser.add_argument("--series", nargs="+", default=list(SERIES))
    parser.add_argument("--rpc", nargs="+", default=list(DEFAULT_RPCS),
                        help="Polygon JSON-RPC endpoints, tried in turn when one is busy")
    parser.add_argument("--data", type=Path, default=DATA)
    args = parser.parse_args()
    fetch(Rpc(args.rpc), args.series, args.first, args.last, args.data)


if __name__ == "__main__":
    main()
