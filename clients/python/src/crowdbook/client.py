"""Trading in a served crowdbook market: the connection, the seat's own books, and bots."""

from __future__ import annotations

import heapq
import itertools
import select
import socket
from dataclasses import dataclass
from typing import Optional

from . import protocol
from .protocol import (AUCTIONS, Accepted, Cancel, Cancelled, ClientMessage, Clock, Depth, End,
                       Error, Filled, Hello, Indicative, Level, Modified, Modify, NewOrder,
                       OrderEvent, PhaseChange, Rejected, ServerMessage, Side, Start,
                       TimeInForce, Top, Trade, Welcome)


@dataclass
class Order:
    """One of the seat's own orders, as the client knows it."""

    id: int
    side: Side
    order_type: str
    price: Optional[int]
    leaves: int  # open quantity as last reported; the full size until acknowledged
    order_id: int = 0  # the exchange's id, 0 until acknowledged
    acknowledged: bool = False
    cancel_requested: bool = False


class Ledger:
    """The seat's cash, position, fees and orders, built only from the requests the client sent
    and the order events it received. With latency it lags the exchange: orders are in flight
    until acknowledged, and a cancel can cross a fill."""

    def __init__(self, cash: int = 0, position: int = 0, fees: int = 0) -> None:
        self.cash = cash
        self.position = position
        self.fees = fees  # in thousandths of a tick-lot, net of rebates
        self.orders: dict[int, Order] = {}  # open or in flight, by the client's id

    def record(self, message: ClientMessage) -> None:
        """Records a request the client is sending. Raises ValueError for a new order whose id
        is already live."""
        if isinstance(message, NewOrder):
            if message.id in self.orders:
                raise ValueError(f"order id {message.id} is already live")
            self.orders[message.id] = Order(id=message.id, side=message.side,
                                            order_type=message.order_type, price=message.price,
                                            leaves=message.quantity)
        elif isinstance(message, Cancel) and message.id in self.orders:
            self.orders[message.id].cancel_requested = True
        # A modify changes nothing until the exchange confirms it.

    def apply(self, event: OrderEvent) -> None:
        """Applies one of the seat's order events. Raises KeyError for an event about an order
        the client never sent."""
        if isinstance(event, Accepted):
            order = self.orders[event.id]
            order.acknowledged = True
            order.order_id = event.order_id
        elif isinstance(event, Rejected):
            if event.request == "new":
                del self.orders[event.id]
            elif event.request == "cancel" and event.id in self.orders:
                # The cancel failed; an order that is gone finished while it was on its way.
                self.orders[event.id].cancel_requested = False
        elif isinstance(event, Modified):
            order = self.orders[event.id]
            order.price = event.price
            order.leaves = event.quantity
        elif isinstance(event, Filled):
            order = self.orders[event.id]
            sign = 1 if event.side == "buy" else -1
            self.position += sign * event.quantity
            self.cash -= sign * event.price * event.quantity
            self.fees += event.fee
            order.leaves = event.leaves
            if order.leaves == 0:
                del self.orders[event.id]
        elif isinstance(event, Cancelled):
            del self.orders[event.id]

    def open_quantity(self, side: Side) -> int:
        """Open quantity on one side, including orders still in flight."""
        return sum(order.leaves for order in self.orders.values() if order.side == side)


class Book:
    """The public market as the client has heard it: best prices, depth, the last trade, and in
    a market with a trading day its phase and, during an auction, the indicative price."""

    def __init__(self) -> None:
        self.bid: Optional[Level] = None
        self.ask: Optional[Level] = None
        self.bids: tuple[Level, ...] = ()  # best first; empty without a depth feed
        self.asks: tuple[Level, ...] = ()
        self.last_price: Optional[int] = None
        self.trades = 0
        self.volume = 0
        self.phase = "continuous"
        self.indicative: Optional[Indicative] = None

    @property
    def in_auction(self) -> bool:
        return self.phase in AUCTIONS

    def apply(self, message: ServerMessage) -> None:
        if isinstance(message, PhaseChange):
            self.phase = message.phase
            if not self.in_auction:
                self.indicative = None
        elif isinstance(message, Indicative):
            self.indicative = message if message.price is not None else None
        elif isinstance(message, Trade):
            self.last_price = message.price
            self.trades += 1
            self.volume += message.quantity
        elif isinstance(message, Top):
            self.bid, self.ask = message.bid, message.ask
        elif isinstance(message, Depth):
            self.bids, self.asks = message.bids, message.asks

    @property
    def mid(self) -> Optional[float]:
        if self.bid is None or self.ask is None:
            return None
        return (self.bid.price + self.ask.price) / 2


class Connection:
    """A TCP connection to a crowdbook server that sends and receives whole messages."""

    def __init__(self, host: str = "127.0.0.1", port: int = 7878, timeout: float = 10.0) -> None:
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buffer = b""
        self.closed = False

    def send(self, message: ClientMessage) -> bool:
        """Sends a message. Returns False, and marks the connection closed, if the server has
        gone."""
        if self.closed:
            return False
        try:
            self._socket.sendall(protocol.encode(message).encode("ascii"))
        except (BrokenPipeError, ConnectionResetError):
            self.closed = True
            return False
        return True

    def receive(self, timeout: float) -> list[ServerMessage]:
        """The messages that arrive within `timeout` seconds, as soon as there is at least one;
        none if the time runs out or the server has closed the connection."""
        messages: list[ServerMessage] = []
        while not messages and not self.closed:
            ready, _, _ = select.select([self._socket], [], [], timeout)
            if not ready:
                break
            try:
                chunk = self._socket.recv(65536)
            except ConnectionResetError:
                chunk = b""
            if not chunk:
                self.closed = True
                break
            self._buffer += chunk
            *lines, self._buffer = self._buffer.split(b"\n")
            messages.extend(protocol.decode(line.decode("ascii")) for line in lines)
        return messages

    def close(self) -> None:
        self._socket.close()
        self.closed = True


class Bot:
    """A trading program for one seat. Override the callbacks you need and act through the
    methods; `run` connects it and calls them as messages arrive.

    Times are the market's simulated nanoseconds. Each callback runs after the message has been
    applied, so `ledger` and `book` already include it."""

    def __init__(self) -> None:
        self.ledger = Ledger()
        self.book = Book()
        self.settings: Optional[Welcome] = None
        self.now = 0
        self.started = False
        self.ended: Optional[End] = None
        self._connection: Optional[Connection] = None
        self._next_id = 1
        self._timers: list[tuple[int, int, object]] = []
        self._sequence = itertools.count()

    # Callbacks.

    def on_start(self) -> None: ...
    def on_wakeup(self, tag: object) -> None: ...
    def on_trade(self, trade: Trade) -> None: ...
    def on_top(self, top: Top) -> None: ...
    def on_depth(self, depth: Depth) -> None: ...
    def on_phase(self, phase: PhaseChange) -> None: ...
    def on_indicative(self, indicative: Indicative) -> None: ...
    def on_accepted(self, event: Accepted) -> None: ...
    def on_rejected(self, event: Rejected) -> None: ...
    def on_modified(self, event: Modified) -> None: ...
    def on_filled(self, event: Filled) -> None: ...
    def on_cancelled(self, event: Cancelled) -> None: ...
    def on_error(self, error: Error) -> None: ...
    def on_end(self, end: End) -> None: ...

    # Actions. Each returns the id that names the order in later events.

    def buy(self, price: int, quantity: int,
            time_in_force: TimeInForce = "good-till-cancel") -> int:
        return self._submit("buy", "limit", quantity, price, time_in_force)

    def sell(self, price: int, quantity: int,
             time_in_force: TimeInForce = "good-till-cancel") -> int:
        return self._submit("sell", "limit", quantity, price, time_in_force)

    def buy_market(self, quantity: int) -> int:
        return self._submit("buy", "market", quantity, None, None)

    def sell_market(self, quantity: int) -> int:
        return self._submit("sell", "market", quantity, None, None)

    def cancel(self, order_id: int) -> None:
        self._send(Cancel(id=order_id))

    def cancel_all(self) -> None:
        """Cancels every open order whose cancel has not been asked for already."""
        for order in list(self.ledger.orders.values()):
            if not order.cancel_requested:
                self.cancel(order.id)

    def modify(self, order_id: int, price: int, quantity: int) -> None:
        self._send(Modify(id=order_id, price=price, quantity=quantity))

    def wake_after(self, delay: int, tag: object = None) -> None:
        """Asks for on_wakeup once the market's time has moved on by `delay` nanoseconds. Wakeups
        happen as messages bring news of the time, about ten times a second at the least."""
        heapq.heappush(self._timers, (self.now + delay, next(self._sequence), tag))

    # Running.

    def _submit(self, side: Side, order_type: str, quantity: int, price: Optional[int],
                time_in_force: Optional[TimeInForce]) -> int:
        order = NewOrder(id=self._next_id, side=side, order_type=order_type,  # type: ignore
                         quantity=quantity, price=price, time_in_force=time_in_force)
        self._send(order)
        self._next_id += 1
        return order.id

    def _send(self, message: ClientMessage) -> None:
        if self._connection is None or not self.started or self.ended is not None:
            raise RuntimeError("orders can only be sent while the market is open")
        # Once the server has gone nothing more reaches the market, so nothing is recorded.
        if self._connection.send(message):
            self.ledger.record(message)

    def _dispatch(self, message: ServerMessage) -> None:
        if isinstance(message, Welcome):
            self.settings = message
            account = message.account
            self.ledger = Ledger(account.cash, account.position, account.fees)
            for order in message.orders:
                self.ledger.orders[order.id] = Order(
                    id=order.id, side=order.side, order_type=order.order_type, price=order.price,
                    leaves=order.leaves, order_id=order.order_id,
                    acknowledged=order.acknowledged, cancel_requested=order.cancel_requested)
                self._next_id = max(self._next_id, order.id + 1)
            self.now = message.time
            self.book.phase = message.phase
            if message.started:
                self.started = True
                self.on_start()
            return
        if isinstance(message, Error):
            self.on_error(message)
            return
        self.now = message.time
        if isinstance(message, Start):
            self.started = True
            self.on_start()
        elif isinstance(message, End):
            self.ended = message
            self.on_end(message)
            return
        elif isinstance(message, (Accepted, Rejected, Modified, Filled, Cancelled)):
            self.ledger.apply(message)
            handler = {Accepted: self.on_accepted, Rejected: self.on_rejected,
                       Modified: self.on_modified, Filled: self.on_filled,
                       Cancelled: self.on_cancelled}[type(message)]
            handler(message)  # type: ignore[operator]
        elif isinstance(message, (Trade, Top, Depth, PhaseChange, Indicative)):
            self.book.apply(message)
            if isinstance(message, Trade):
                self.on_trade(message)
            elif isinstance(message, Top):
                self.on_top(message)
            elif isinstance(message, Depth):
                self.on_depth(message)
            elif isinstance(message, PhaseChange):
                self.on_phase(message)
            else:
                self.on_indicative(message)
        while self._timers and self._timers[0][0] <= self.now and self.ended is None:
            _, _, tag = heapq.heappop(self._timers)
            self.on_wakeup(tag)


def run(bot: Bot, host: str = "127.0.0.1", port: int = 7878, seat: str = "you",
        token: Optional[str] = None) -> Optional[End]:
    """Connects the bot to a server, claims the seat and runs the bot until the session ends.
    Returns the seat's results, or None if the connection closed first. Raises ConnectionError if
    the server turns the seat away."""
    connection = Connection(host, port)
    bot._connection = connection
    try:
        connection.send(Hello(seat=seat, token=token))
        while bot.ended is None and not connection.closed:
            for message in connection.receive(timeout=1.0):
                bot._dispatch(message)
                if isinstance(message, Error) and message.fatal:
                    raise ConnectionError(f"the server closed the connection: {message.message}")
                if bot.ended is not None:
                    break
        return bot.ended
    finally:
        connection.close()
