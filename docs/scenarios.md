# Scenario files

A scenario is a TOML file describing a market to simulate: the run's settings, optionally the
asset's true value, and groups of agents. Run one with:

```bash
./build/dev/apps/crowdbook run examples/scenarios/market_maker.toml --seed 42 --log run.csv
```

| Option | Meaning |
|---|---|
| `--seed N`, `--duration D` | Override the file's values |
| `--log FILE` | Write every request the exchange receives and every event it produces as CSV ([the event log](#the-event-log)) |
| `--log-only KIND,...` | Keep only these kinds of log rows, such as `trade,top_of_book` |
| `--prices FILE` | Write the best bid, best ask and last trade price at regular times ([samples](#price-and-depth-samples)) |
| `--depth FILE` | Write the book's best levels at regular times; needs a depth feed ([samples](#price-and-depth-samples)) |
| `--sample-interval D` | How often `--prices` and `--depth` sample; `"1s"` unless given |
| `--json FILE` | Write the results for analysis scripts ([results as JSON](#results-as-json)) |

`crowdbook agents` lists the agent types. `crowdbook play` and `crowdbook replay` run a scenario
with a person trading in it; see [playing and replaying](#playing-and-replaying).

Durations are written as text with a unit: `"250ns"`, `"50us"`, `"1.5ms"`, `"2s"`. Prices are in
ticks and quantities in lots. Any key crowdbook does not know is an error, so a typo stops the run
instead of silently using a default.

## Top level

| Key | Type | Default | Meaning |
|---|---|---|---|
| `seed` | integer | `1` | The run's seed; the same seed always gives the same run |
| `duration` | duration | `"60s"` | How much simulated time to run |
| `reference_price` | integer | `10000` | Where agents anchor before they have seen any quotes |

## `[fundamental]` (optional)

The asset's true value, which informed agents observe with noise. It follows an
Ornstein–Uhlenbeck process, or a random walk when `mean_reversion` is 0.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `initial` | number | the reference price | Starting value, and the level it reverts to |
| `mean_reversion` | number | `0` | Per second; 0 makes the value a random walk |
| `volatility` | number | `1` | Ticks per square root of a second |
| `step` | duration | `"100ms"` | How often the value changes |

## `[exchange]` (optional)

Rules and market data that apply to everyone.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `depth_levels` | integer | `0` | Price levels per side in the public depth feed, up to 1,000; 0 publishes only the best bid and ask |
| `maker_fee` | number | `0` | Ticks per lot paid by the owner of the resting order in every trade; negative is a rebate |
| `taker_fee` | number | `0` | Ticks per lot paid by the owner of the incoming order |

Fees take at most three decimals, such as `-0.25`, and `maker_fee + taker_fee` must not be
negative: no exchange pays out more in rebates than it collects. Fees are kept apart from cash,
which only trades move, and every fill reports its own fee.

A depth feed lets agents see the book beyond the best prices, both in `context.market()` and, for
agents that stream market data, in `onDepth`. It costs time: ten levels add about half again to
the run time of the thousand-trader example market, so it is off unless a scenario asks for it.

## `[participant]` (optional)

The account and latency of the person trading in the market with `crowdbook play`. Agents' runs
ignore it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `latency` | table | no delay | `to_exchange`, `from_exchange` and `jitter`, all durations |
| `account` | table | no limits | `initial_cash`, `initial_position`, `max_position`, `max_order_quantity` |

## `[[agents]]`

Each `[[agents]]` table is a group of agents of one type that share their settings. Every agent
still gets its own id and its own random streams.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | text | required | A built-in type below, or one you register |
| `name` | text | the type | Label in the results |
| `count` | integer | `1` | How many agents the group has |
| `start` | duration | the start of the run | When the agents start |
| `latency` | table | no delay | `to_exchange`, `from_exchange` and `jitter`, all durations |
| `account` | table | no limits | `initial_cash`, `initial_position`, `max_position`, `max_order_quantity` |

`max_position` is checked as if every open order on a side filled, and short selling is allowed
within it. Cash has no limit. Every other key in the table is a parameter of the agent type.

## Built-in agents

The market maker receives every trade and quote change as it happens. The other three read the
market on demand when they act, which keeps crowds of thousands fast; either way, each sees the
market only after its own `from_exchange` latency. Agents that act on a timer (the market maker,
momentum and informed traders) start it at a random point within the first interval, so a group
of them does not act in lockstep.

### `zero_intelligence`

Random limit and market orders at Poisson times, after Farmer, Patelli and Zovko (2005). Each
limit order that rests is cancelled after its own random lifetime.

| Parameter | Default | Meaning |
|---|---|---|
| `limit_rate` | `2.0` | Limit orders per second |
| `market_rate` | `0.5` | Market orders per second |
| `cancel_rate` | `0.2` | Per resting order per second: an order that never fills rests 1/`cancel_rate` seconds on average; `0` keeps orders until they fill |
| `max_offset` | `10` | Limit prices are 1 to `max_offset` ticks inside the opposite best quote |
| `min_size`, `max_size` | `1`, `10` | Range of order sizes, in lots |

### `market_maker`

Quotes a bid and an ask around a reservation price that leans against its inventory, after
Avellaneda and Stoikov (2008): `r = s − q·γ·σ²·τ`, with a half spread of
`γσ²τ/2 + ln(1 + γ/k)/γ`. Its fair price `s` is a running average of trade prices.

| Parameter | Default | Meaning |
|---|---|---|
| `risk_aversion` | `0.005` | γ: how hard inventory skews the quotes |
| `volatility` | `3.0` | σ: ticks per square root of a second |
| `intensity` | `0.3` | k: the base half spread is about 1/k ticks |
| `horizon` | `"1s"` | τ: the holding period inventory risk is priced over |
| `quote_size` | `5` | Lots per quote |
| `max_inventory` | `50` | Never quotes a side that could take its position past this |
| `requote_interval` | `"100ms"` | How often it requotes; it also requotes after each of its fills |
| `fair_value_weight` | `0.2` | How far each trade moves its fair price toward the trade's price |
| `post_only` | `false` | Quote with post-only orders, a tick off the far side of the book it last saw, so it only ever adds liquidity |

Keep γ·σ²·τ, the skew per lot of inventory, small. When other traders anchor on the market maker's
quotes, a large skew drags the whole market against its own inventory.

### `momentum`

Compares a fast and a slow moving average of the price and trades with market orders in the
direction of the trend.

| Parameter | Default | Meaning |
|---|---|---|
| `interval` | `"50ms"` | How often it checks the trend |
| `fast_half_life` | `"200ms"` | Half-life of the fast moving average |
| `slow_half_life` | `"2s"` | Half-life of the slow moving average |
| `threshold` | `2.0` | Ticks the fast average must lead the slow one by |
| `order_size` | `5` | Lots per order |
| `max_position` | `50` | Largest position it builds, counting orders in flight |

### `informed`

Observes the fundamental value with noise and trades when the book is far enough from it, using
immediate-or-cancel orders priced to keep its edge. Needs a `[fundamental]` section.

To hold the price to the value over a long run, informed traders must absorb the other traders'
net order flow, so give them room: with small position limits they fill up, and the price drifts
away from the value. [large_market.toml](../examples/scenarios/large_market.toml) shows a setting
that holds for a simulated day.

| Parameter | Default | Meaning |
|---|---|---|
| `interval` | `"100ms"` | How often it looks at the value |
| `noise` | `1.0` | Ticks; standard deviation of each look at the value |
| `threshold` | `3.0` | Ticks of edge needed before trading |
| `order_size` | `5` | Lots per order |
| `max_position` | `50` | Largest position it builds, counting orders in flight |

## Your own agents

Subclass `crowdbook::Agent`, register a factory for it under a name, and use that name as the
`type` of an `[[agents]]` table. [examples/custom_agent.cpp](../examples/custom_agent.cpp) does this
in about 40 lines; the factory reads its parameters from `crowdbook::Parameters`, and anything it
does not read is reported as an unknown parameter. An agent that only needs the market when it
acts should override `marketData()` to return `MarketDataMode::Snapshot` and call
`context.market()`, instead of receiving every update.

## The event log

`--log` writes one CSV row per request or event, in the order the exchange processed them.
Columns that do not apply to a row are empty.

| Column | Meaning |
|---|---|
| `time` | Nanoseconds since the start of the run |
| `kind` | `new`, `cancel`, `modify` (requests as they reach the exchange); `accepted`, `rejected`, `modified`, `filled`, `cancelled` (reports to one agent); `trade`, `top_of_book` (public) |
| `agent`, `client_order_id`, `order_id` | Who and which order |
| `side`, `type`, `time_in_force` | `buy` or `sell`; `limit` or `market`; `good-till-cancel`, `immediate-or-cancel` or `post-only`. For trades, the side is the aggressor's |
| `price`, `quantity`, `leaves` | Ticks and lots; `leaves` is the open quantity after a fill |
| `liquidity`, `fee` | For fills: `maker` or `taker`, and the fill's fee in tick-lots (negative for a rebate) |
| `request`, `reason` | For rejections, the request kind and why; for cancellations, why |
| `bid_price`, `bid_quantity`, `ask_price`, `ask_quantity` | For `top_of_book` |

Updates of the depth feed are not logged: the log's orders already determine the whole book, and
`--depth` records the levels at regular times.

## Price and depth samples

`--prices` writes `time,bid,ask,last_trade` every `--sample-interval`, from time 0 to the end of
the run: the best bid and ask as the market stood at that time, after everything that happened at
it, and the price of the last trade so far. A field is empty while there is no such price. A
simulated day sampled every second is 86,400 rows, about 3 MB, where the full event log of the
thousand-trader example market is about 12 GB, so long runs for return statistics use this
instead of `--log`.

`--depth` writes the depth feed the same way, with the columns `time`, then `bid_price_1`,
`bid_quantity_1` and so on to the feed's depth, then the same for asks. Levels the book does not
have are empty.

## Results as JSON

`--json` writes the run's settings and results as one object:

```json
{
  "seed": 11,
  "duration_ns": 60000000000,
  "reference_price": 10000,
  "trades": 2387,
  "volume": 8994,
  "last_price": 9978,
  "final_value": 9977.291935501687,
  "groups": [
    {"name": "maker", "type": "market_maker", "agents": 1, "agent_ids": [1], "traded": 3287,
     "initial_cash": 0, "initial_position": 0, "cash": -42719, "position": 5, "pnl": 7171,
     "fees": 0},
    ...
  ]
}
```

| Field | Meaning |
|---|---|
| `trades`, `volume` | Trades printed and lots traded |
| `last_price` | The last trade's price, or the reference price if nothing traded |
| `final_value` | The fundamental value at the end, or `null` without a `[fundamental]` section |
| `agent_ids` | The group's agents, as they appear in the event log's `agent` column |
| `traded` | Lots the group bought plus lots it sold |
| `cash`, `pnl` | In tick-lots; `pnl` is the change in cash plus the change in position valued at `last_price`, before fees |
| `fees` | Fees paid net of rebates, in tick-lots |

## Playing and replaying

```bash
./build/release/apps/crowdbook play examples/scenarios/playable.toml --record session.toml
./build/release/apps/crowdbook replay session.toml --log session.csv
```

`play` runs the scenario in real time on a trading screen in the terminal, with you as the
scenario's participant, until the scenario's duration or until you quit. It takes `--seed`,
`--duration`, `--log` and `--log-only` like `run`, and:

| Option | Meaning |
|---|---|
| `--speed X` | How many simulated seconds pass per second; `1` unless given. `[` and `]` change it as you play |
| `--record FILE` | Write the session, for `replay` |

| Key | Does |
|---|---|
| up, down, page up, page down | Move the price you trade at by one or ten ticks |
| `m` | Move it to the middle of the market |
| `b`, `s` | Bid or offer at that price |
| `B`, `S` | Buy or sell at the market |
| `+`, `-` | Change the size of your orders |
| `c`, `C` | Cancel your orders at that price, or all of them |
| `[`, `]` | Slower or faster |
| space | Pause or carry on |
| `q` | Stop and see the results |

The ladder shows the depth feed, so the scenario needs `depth_levels` in its `[exchange]` section
to show more than the best prices. Your orders are a participant agent's: they wait out its
latency, count against its limits and pay the exchange's fees, and you are reported as the group
`you`.

`replay` plays a recorded session back: the same market with the same seed, your orders at the
same simulated nanoseconds, up to where you stopped. Its event log is the session's, byte for
byte. It takes the output options of `run`; the seed and the duration come from the session.

### Session files

A session is a TOML file holding everything a replay needs:

| Key | Meaning |
|---|---|
| `session_version` | `1` |
| `seed` | The seed the session ran with |
| `end_ns` | When it stopped, in simulated nanoseconds |
| `scenario` | The scenario file's text, so a session replays even if the file changes |
| `actions` | Your requests in time order, one table each: `time_ns`, `instrument` (always 0 for now), `request` (`new`, `cancel` or `modify`) and the request's fields: `client_order_id`, `side`, `type`, `time_in_force`, `price`, `quantity` |

A replay checks that each new order gets the client order id the session recorded, and stops with
an error if it does not, which would mean the market had come out differently.
