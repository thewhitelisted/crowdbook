"""Reading crowdbook's CSV event logs."""

from pathlib import Path

import polars as pl

_TEXT = pl.Utf8
_NUMBER = pl.Int64

# Every column of the log, in order. Columns that do not apply to a row are empty, so all of them
# are nullable.
SCHEMA = {
    "time": _NUMBER,
    "kind": _TEXT,
    "agent": _NUMBER,
    "client_order_id": _NUMBER,
    "order_id": _NUMBER,
    "side": _TEXT,
    "type": _TEXT,
    "time_in_force": _TEXT,
    "price": _NUMBER,
    "quantity": _NUMBER,
    "leaves": _NUMBER,
    "liquidity": _TEXT,
    "fee": pl.Float64,  # in tick-lots, on fills
    "request": _TEXT,
    "reason": _TEXT,
    "bid_price": _NUMBER,
    "bid_quantity": _NUMBER,
    "ask_price": _NUMBER,
    "ask_quantity": _NUMBER,
    "parent": _NUMBER,  # on new orders that are part of a larger one
}


def read_log(path: Path) -> pl.DataFrame:
    """Reads a whole event log, one row per request or event."""
    return pl.read_csv(path, schema=SCHEMA)


def quotes(log: pl.DataFrame) -> pl.DataFrame:
    """Top-of-book changes, with the mid and spread wherever both sides are quoted."""
    return (
        log.filter(pl.col("kind") == "top_of_book")
        .select("time", "bid_price", "bid_quantity", "ask_price", "ask_quantity")
        .with_columns(
            mid=(pl.col("bid_price") + pl.col("ask_price")) / 2,
            spread=pl.col("ask_price") - pl.col("bid_price"),
        )
    )


def trades(log: pl.DataFrame) -> pl.DataFrame:
    """Trade prints. `side` is the side of the order that took liquidity."""
    return log.filter(pl.col("kind") == "trade").select("time", "price", "quantity", "side")


def aggressive_orders(log: pl.DataFrame) -> pl.DataFrame:
    """Orders that took liquidity, one row each: when the order first traded, the agent that
    sent it, its side and the lots it took. Needs the log's `filled` rows."""
    return (
        log.filter((pl.col("kind") == "filled") & (pl.col("liquidity") == "taker"))
        .group_by("agent", "order_id")
        .agg(pl.col("time").min(), pl.col("side").first(), pl.col("quantity").sum())
        .select("time", "agent", "order_id", "side", "quantity")
        .sort("time", "agent", "order_id")
    )


def executions(log: pl.DataFrame) -> pl.DataFrame:
    """One row per execution: when it traded, its price and lots, the agent whose resting order
    it filled with that agent's side, and the agent that took liquidity. Needs the log's `filled`
    rows, where each execution's maker fill comes just before its taker fill."""
    fills = log.filter(pl.col("kind") == "filled").with_row_index("row")
    makers = fills.filter(pl.col("liquidity") == "maker").select(
        "row",
        "time",
        "price",
        "quantity",
        pl.col("agent").alias("maker"),
        pl.col("side").alias("maker_side"),
    )
    takers = fills.filter(pl.col("liquidity") == "taker").select(
        (pl.col("row") - 1).alias("row"), pl.col("agent").alias("taker")
    )
    return makers.join(takers, on="row").sort("row").drop("row")


def parent_orders(log: pl.DataFrame) -> pl.DataFrame:
    """One row per parent order, put together from its children: the agent, the agent's id for
    the parent, its side, when its first child reached the exchange, when its last child traded,
    the lots traded and the children sent. Needs the log's `new` rows, which carry the parent, and
    its `filled` rows. A parent with nothing traded has no end."""
    children = log.filter((pl.col("kind") == "new") & pl.col("parent").is_not_null()).select(
        "agent", "client_order_id", "parent", "side", "time"
    )
    fills = (
        log.filter(pl.col("kind") == "filled")
        .group_by("agent", "client_order_id")
        .agg(pl.col("time").max().alias("filled_at"), pl.col("quantity").sum().alias("lots"))
    )
    return (
        children.join(fills, on=["agent", "client_order_id"], how="left")
        .group_by("agent", "parent")
        .agg(
            pl.col("side").first(),
            pl.col("time").min().alias("start"),
            pl.col("filled_at").max().alias("end"),
            pl.col("lots").sum(),
            pl.len().alias("children"),
        )
        .sort("start", "agent")
    )


def read_prices(path: Path) -> pl.DataFrame:
    """Reads a price file written by `crowdbook run --prices`, adding the mid where both sides
    are quoted."""
    prices = pl.read_csv(
        path,
        schema={"time": _NUMBER, "bid": _NUMBER, "ask": _NUMBER, "last_trade": _NUMBER},
    )
    return prices.with_columns(mid=(pl.col("bid") + pl.col("ask")) / 2)
