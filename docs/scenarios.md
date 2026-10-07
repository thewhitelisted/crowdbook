# Scenario files

A scenario is a TOML file describing a market to simulate: the run's settings, optionally the
asset's true value, and groups of agents. Run one with:

```bash
./build/dev/apps/crowdbook run examples/scenarios/market_maker.toml --seed 42 --log run.csv
```

`--seed` and `--duration` override the file's values, and `--log` writes every request the
exchange receives and every event it produces as CSV. `crowdbook agents` lists the agent types.

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

### `zero_intelligence`

Random limit orders, market orders and cancellations at Poisson times, after Farmer, Patelli and
Zovko (2005).

| Parameter | Default | Meaning |
|---|---|---|
| `limit_rate` | `2.0` | Limit orders per second |
| `market_rate` | `0.5` | Market orders per second |
| `cancel_rate` | `1.0` | Cancellations of its own orders per second |
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
does not read is reported as an unknown parameter.

## The event log

`--log` writes one CSV row per request or event, in the order the exchange processed them.
Columns that do not apply to a row are empty.

| Column | Meaning |
|---|---|
| `time` | Nanoseconds since the start of the run |
| `kind` | `new`, `cancel`, `modify` (requests as they reach the exchange); `accepted`, `rejected`, `modified`, `filled`, `cancelled` (reports to one agent); `trade`, `top_of_book` (public) |
| `agent`, `client_order_id`, `order_id` | Who and which order |
| `side`, `type`, `time_in_force` | `buy` or `sell`; `limit` or `market`; for trades, the side is the aggressor's |
| `price`, `quantity`, `leaves` | Ticks and lots; `leaves` is the open quantity after a fill |
| `liquidity` | `maker` or `taker`, for fills |
| `request`, `reason` | For rejections, the request kind and why; for cancellations, why |
| `bid_price`, `bid_quantity`, `ask_price`, `ask_quantity` | For `top_of_book` |
