# crowdbook protocol

How a client trades in a market run by `crowdbook serve`. [design.md](design.md#gateway) explains
the choices behind it.

Protocol version: **1**.

## Encoding

A connection is a TCP stream of messages in both directions, one JSON object per line, each line
ending in `\n`. Every message has a `"type"`.

- Numbers are whole numbers that fit a signed 64-bit integer: no fractions and no exponents. Times
  are simulated nanoseconds since the start of the market, prices are ticks, quantities are lots,
  cash is tick-lots and fees are thousandths of a tick-lot. JavaScript clients should read them as
  `BigInt` if values beyond 2<sup>53</sup> are possible; times reach that after 104 simulated days.
- Strings are ASCII. The escapes `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t` and `\u0000` to
  `\u007f` are understood.
- A client message may be at most 4,096 bytes, including its newline, and may not nest objects or
  arrays more than eight deep. Unknown fields, repeated fields and fields of the wrong type are
  errors.
- The server writes fields in the order listed here, but clients should not depend on it.

## Session

1. The client connects and sends `hello` within five seconds.
2. The server answers with `welcome`, or with a fatal `error` and closes the connection.
3. When every seat is claimed, the server sends `start` to everyone and the clock runs. A client
   that claims its seat after the start gets `"started": true` in its welcome instead.
4. The client sends orders, cancels and modifies; the server sends its own order events and the
   public market data.
5. When the market's time is up, or the server is stopped, it sends `end` and closes the
   connection.

If a connection closes, every open order of its seat is cancelled. The seat can be claimed again
with a new `hello`.

## Client messages

### hello

```json
{"type":"hello","protocol":1,"seat":"alice","token":"s3cret"}
```

| Field | | |
|---|---|---|
| `protocol` | required | must be 1 |
| `seat` | required | the seat's name: 1 to 32 letters, digits, `-` or `_` |
| `token` | optional | the seat's token, when the server was started with a token file; up to 128 printable ASCII characters |

### new

```json
{"type":"new","id":17,"side":"buy","order_type":"limit","time_in_force":"good-till-cancel","price":10003,"quantity":5}
```

| Field | | |
|---|---|---|
| `id` | required | the client's own id for the order, positive; must not belong to one of the seat's live orders |
| `side` | required | `buy` or `sell` |
| `order_type` | required | `limit` or `market` |
| `time_in_force` | limit orders | `good-till-cancel`, `immediate-or-cancel` or `post-only` |
| `price` | limit orders | in ticks |
| `quantity` | required | in lots |
| `parent` | optional | the client's id for a larger order this one is part of; kept in the event log |

A market order must not have `time_in_force` or `price`.

### cancel

```json
{"type":"cancel","id":17}
```

### modify

```json
{"type":"modify","id":17,"price":10004,"quantity":3}
```

Sets the order's price and open quantity. Reducing the quantity at the same price keeps the order's
place in the queue; any other change loses it.

A live order is one the client has sent and not yet seen filled, cancelled or rejected. Its id can
be used again once it is no longer live.

## Server messages

Every server message except `welcome` and `error` carries `time`: the simulated time at which it
reached the seat. Order events and market data reach a seat one `from_exchange` latency after the
exchange produced them, in the order the exchange produced them.

### welcome

```json
{"type":"welcome","protocol":1,"seat":"alice","started":false,"time":0,"duration":60000000000,
 "reference_price":10000,"depth_levels":10,"maker_fee":0,"taker_fee":0,
 "latency":{"to_exchange":1000000,"from_exchange":1000000,"jitter":0},
 "account":{"initial_cash":0,"initial_position":0,"cash":0,"position":0,"fees":0,
            "max_position":50,"max_order_quantity":10},
 "orders":[]}
```

Sent on one line. `duration` is the market's length, `depth_levels` the levels per side in
`depth` messages (0 for none), and `maker_fee` and `taker_fee` are per lot. `account` gives the
seat's starting balances, its balances now and its limits, and `orders` its live orders, each as
`{"id","order_id","side","order_type","price","leaves","acknowledged","cancel_requested"}`, so a
client claiming a seat again can carry on.

### start

```json
{"type":"start","time":0}
```

### accepted

```json
{"type":"accepted","time":1000000,"id":17,"order_id":4211,"side":"buy","order_type":"limit","time_in_force":"good-till-cancel","price":10003,"quantity":5}
```

`order_id` is the exchange's id for the order.

### rejected

```json
{"type":"rejected","time":1000000,"id":17,"request":"new","reason":"position-limit"}
```

`request` is `new`, `cancel` or `modify`. `reason` is one of `non-positive-quantity`,
`invalid-price`, `order-size-limit`, `position-limit`, `duplicate-client-order-id`,
`unknown-order-id` or `post-only-would-trade`. The gateway itself rejects a new order whose id is
live and a cancel or modify whose id is not, at once and without the latency; the exchange rejects
everything else.

### modified

```json
{"type":"modified","time":2000000,"id":17,"order_id":4211,"price":10004,"quantity":3}
```

### filled

```json
{"type":"filled","time":3000000,"id":17,"order_id":4211,"side":"buy","price":10004,"quantity":2,"leaves":1,"liquidity":"maker","fee":0}
```

`leaves` is what is still open; `liquidity` is `maker` or `taker`; `fee` is charged for this fill,
negative for a rebate.

### cancelled

```json
{"type":"cancelled","time":4000000,"id":17,"order_id":4211,"quantity":1,"reason":"requested"}
```

`reason` is `requested`, `immediate-or-cancel` (the part of an immediate-or-cancel or market order
that could not trade) or `self-trade`.

### trade

```json
{"type":"trade","time":3000000,"price":10004,"quantity":2,"aggressor":"sell"}
```

Every trade in the market, anonymous.

### top

```json
{"type":"top","time":3000000,"bid":{"price":10003,"quantity":12,"orders":3},"ask":null}
```

The best bid and ask, sent when either changes; `null` for an empty side.

### depth

```json
{"type":"depth","time":3000000,"bids":[{"price":10003,"quantity":12,"orders":3}],"asks":[]}
```

The best `depth_levels` levels on each side, best first, sent when any of them changes. Only for
markets with a depth feed.

### clock

```json
{"type":"clock","time":5000000000}
```

The market's time, sent about ten times a second of wall-clock time when nothing else has been.

### end

```json
{"type":"end","time":60000000000,"cash":-50012,"position":5,"fees":1000,"pnl":45}
```

The seat's balances at the end. `pnl` is the change in cash plus the change in position valued at
the last trade price, before fees, in tick-lots.

### error

```json
{"type":"error","message":"unknown field 'qty'","fatal":false}
```

A message the server could not use, which it dropped: malformed, too long, or over the
connection's limit of messages per second. With `"fatal": true` the server closes the connection
after it: for a bad or missing `hello`, or a client too slow to read what it is sent.
