"""The messages of the crowdbook protocol and their encoding as JSON lines.

docs/protocol.md in the crowdbook repository specifies them. Client messages are encoded exactly
as the server expects; server messages are decoded leniently, ignoring fields this version does
not know, so that a newer server can add fields without breaking older clients.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Literal, Optional, Union

VERSION = 1

Side = Literal["buy", "sell"]
OrderType = Literal["limit", "market"]
TimeInForce = Literal["good-till-cancel", "immediate-or-cancel", "post-only"]

SIDES = ("buy", "sell")
ORDER_TYPES = ("limit", "market")
TIMES_IN_FORCE = ("good-till-cancel", "immediate-or-cancel", "post-only")


class ProtocolError(ValueError):
    """A line that is not a message this client understands."""


# Client messages.


@dataclass(frozen=True)
class Hello:
    seat: str
    token: Optional[str] = None
    protocol: int = VERSION


@dataclass(frozen=True)
class NewOrder:
    id: int
    side: Side
    order_type: OrderType
    quantity: int
    price: Optional[int] = None  # limit orders only
    time_in_force: Optional[TimeInForce] = None  # limit orders only
    parent: Optional[int] = None


@dataclass(frozen=True)
class Cancel:
    id: int


@dataclass(frozen=True)
class Modify:
    id: int
    price: int
    quantity: int


ClientMessage = Union[Hello, NewOrder, Cancel, Modify]


def encode(message: ClientMessage) -> str:
    """One line of JSON, ending in a newline."""
    if isinstance(message, Hello):
        body: dict = {"type": "hello", "protocol": message.protocol, "seat": message.seat}
        if message.token is not None:
            body["token"] = message.token
    elif isinstance(message, NewOrder):
        if message.side not in SIDES or message.order_type not in ORDER_TYPES:
            raise ValueError(f"not an order: {message}")
        body = {"type": "new", "id": message.id, "side": message.side,
                "order_type": message.order_type}
        if message.order_type == "limit":
            if message.price is None:
                raise ValueError("a limit order needs a price")
            time_in_force = message.time_in_force or "good-till-cancel"
            if time_in_force not in TIMES_IN_FORCE:
                raise ValueError(f"no time in force '{time_in_force}'")
            body["time_in_force"] = time_in_force
            body["price"] = message.price
        elif message.price is not None or message.time_in_force is not None:
            raise ValueError("a market order has no price and no time in force")
        body["quantity"] = message.quantity
        if message.parent is not None:
            body["parent"] = message.parent
    elif isinstance(message, Cancel):
        body = {"type": "cancel", "id": message.id}
    elif isinstance(message, Modify):
        body = {"type": "modify", "id": message.id, "price": message.price,
                "quantity": message.quantity}
    else:
        raise TypeError(f"not a client message: {message!r}")
    return json.dumps(body, separators=(",", ":")) + "\n"


# Server messages.


@dataclass(frozen=True)
class Level:
    price: int
    quantity: int
    orders: int


@dataclass(frozen=True)
class Latency:
    to_exchange: int
    from_exchange: int
    jitter: int


@dataclass(frozen=True)
class Account:
    initial_cash: int
    initial_position: int
    cash: int
    position: int
    fees: int
    max_position: int
    max_order_quantity: int


@dataclass(frozen=True)
class OpenOrder:
    id: int
    order_id: int
    side: Side
    order_type: OrderType
    price: int
    leaves: int
    acknowledged: bool
    cancel_requested: bool


@dataclass(frozen=True)
class Challenge:
    name: str
    briefing: str


@dataclass(frozen=True)
class TargetRule:
    """A large order the seat is asked to work."""

    side: Side
    quantity: int
    benchmark: Literal["vwap", "reference"]
    unfinished_penalty: int  # points per lot not done


@dataclass(frozen=True)
class Scoring:
    """How the seat is scored. Amounts are in points, thousandths of a tick-lot."""

    mark: Literal["last", "value"]
    inventory_penalty: int  # per lot per second held
    close_penalty: int  # per lot held at the end
    max_loss: int  # in tick-lots; 0 for no limit
    target: Optional[TargetRule] = None


@dataclass(frozen=True)
class Score:
    """A seat's score and its parts, in points: total = pnl - inventory - close - paper -
    unfinished."""

    total: int
    pnl: int
    inventory: int
    close: int
    paper: int
    unfinished: int
    unfinished_lots: int
    stopped_at: Optional[int] = None


@dataclass(frozen=True)
class Welcome:
    protocol: int
    seat: str
    started: bool
    time: int
    duration: int
    reference_price: int
    depth_levels: int
    maker_fee: int
    taker_fee: int
    latency: Latency
    account: Account
    orders: tuple[OpenOrder, ...] = ()
    challenge: Optional[Challenge] = None
    scoring: Optional[Scoring] = None
    auction_fee: int = 0
    phase: str = "continuous"
    max_price: int = 1_000_000_000  # the highest limit price: 99 in a prediction market


@dataclass(frozen=True)
class Start:
    time: int


@dataclass(frozen=True)
class Clock:
    time: int


@dataclass(frozen=True)
class Accepted:
    time: int
    id: int
    order_id: int
    side: Side
    order_type: OrderType
    quantity: int
    price: Optional[int] = None
    time_in_force: Optional[TimeInForce] = None


@dataclass(frozen=True)
class Rejected:
    time: int
    id: int
    request: Literal["new", "cancel", "modify"]
    reason: str


@dataclass(frozen=True)
class Modified:
    time: int
    id: int
    order_id: int
    price: int
    quantity: int


@dataclass(frozen=True)
class Filled:
    time: int
    id: int
    order_id: int
    side: Side
    price: int
    quantity: int
    leaves: int
    liquidity: Literal["maker", "taker"]
    fee: int


@dataclass(frozen=True)
class Cancelled:
    time: int
    id: int
    order_id: int
    quantity: int
    reason: Literal["requested", "immediate-or-cancel", "self-trade"]


@dataclass(frozen=True)
class Trade:
    time: int
    price: int
    quantity: int
    aggressor: Side
    auction: bool = False  # part of an auction's uncross


Phase = Literal["opening-auction", "continuous", "halt", "closing-auction", "closed"]
AUCTIONS = ("opening-auction", "halt", "closing-auction")


@dataclass(frozen=True)
class PhaseChange:
    """The market moved to another phase of its trading day; leaving an auction, `price` is
    where it uncrossed."""

    time: int
    phase: Phase
    price: Optional[int] = None


@dataclass(frozen=True)
class Indicative:
    """During an auction: where the book would uncross now; `price` is None while nothing
    would trade."""

    time: int
    price: Optional[int]
    volume: int
    imbalance: int


@dataclass(frozen=True)
class Top:
    time: int
    bid: Optional[Level]
    ask: Optional[Level]


@dataclass(frozen=True)
class Depth:
    time: int
    bids: tuple[Level, ...] = field(default_factory=tuple)
    asks: tuple[Level, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class End:
    time: int
    cash: int
    position: int
    fees: int
    pnl: int
    score: Optional[Score] = None


@dataclass(frozen=True)
class Error:
    message: str
    fatal: bool


OrderEvent = Union[Accepted, Rejected, Modified, Filled, Cancelled]
ServerMessage = Union[Welcome, Start, Clock, Accepted, Rejected, Modified, Filled, Cancelled,
                      Trade, Top, Depth, PhaseChange, Indicative, End, Error]


def _fields(cls, body: dict) -> dict:
    """The body's values for the dataclass's fields, ignoring any others."""
    names = cls.__dataclass_fields__.keys()
    return {name: body[name] for name in names if name in body}


def _level(body: Optional[dict]) -> Optional[Level]:
    return None if body is None else Level(**_fields(Level, body))


def _levels(bodies: list) -> tuple[Level, ...]:
    return tuple(Level(**_fields(Level, body)) for body in bodies)


def _scoring(body: Optional[dict]) -> Optional[Scoring]:
    if body is None:
        return None
    target = body.get("target")
    return Scoring(**{k: v for k, v in _fields(Scoring, body).items() if k != "target"},
                   target=None if target is None else TargetRule(**_fields(TargetRule, target)))


_SIMPLE = {
    "start": Start, "clock": Clock, "accepted": Accepted, "rejected": Rejected,
    "modified": Modified, "filled": Filled, "cancelled": Cancelled, "trade": Trade,
    "phase": PhaseChange, "indicative": Indicative, "error": Error,
}


def decode(line: str) -> ServerMessage:
    """Decodes one line from the server. Raises ProtocolError if it is not a known message."""
    try:
        body = json.loads(line)
        kind = body["type"]
        if kind == "welcome":
            return Welcome(
                **{k: v for k, v in _fields(Welcome, body).items()
                   if k not in ("latency", "account", "orders", "challenge", "scoring")},
                latency=Latency(**_fields(Latency, body["latency"])),
                account=Account(**_fields(Account, body["account"])),
                orders=tuple(OpenOrder(**_fields(OpenOrder, order)) for order in body["orders"]),
                challenge=(None if body.get("challenge") is None
                           else Challenge(**_fields(Challenge, body["challenge"]))),
                scoring=_scoring(body.get("scoring")),
            )
        if kind == "end":
            score = body.get("score")
            return End(**{k: v for k, v in _fields(End, body).items() if k != "score"},
                       score=None if score is None else Score(**_fields(Score, score)))
        if kind == "top":
            return Top(time=body["time"], bid=_level(body["bid"]), ask=_level(body["ask"]))
        if kind == "depth":
            return Depth(time=body["time"], bids=_levels(body["bids"]),
                         asks=_levels(body["asks"]))
        cls = _SIMPLE[kind]
        return cls(**_fields(cls, body))
    except (ValueError, KeyError, TypeError) as error:
        raise ProtocolError(f"cannot decode {line.strip()!r}: {error}") from error
