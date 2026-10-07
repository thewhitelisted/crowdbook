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

Durations are written as text with a unit: `"250ns"`, `"50us"`, `"1.5ms"`, `"2s"`, and are shorter
than `"1000000000s"`, about 31 years. Numbers must be finite: `nan` and `inf` are errors. Prices are
in ticks and quantities in lots. Any key crowdbook does not know is an error, so a typo stops the
run instead of silently using a default.

## Top level

| Key | Type | Default | Meaning |
|---|---|---|---|
| `seed` | integer | `1` | The run's seed; the same seed always gives the same run |
| `duration` | duration | `"60s"` | How much simulated time to run |
| `reference_price` | integer | `10000` | Where agents anchor before they have seen any quotes |

## `[fundamental]` (optional)

The asset's true value, which informed and adaptive agents observe with noise. It follows an
Ornstein–Uhlenbeck process, or a random walk when `mean_reversion` is 0, and jumps when there is
news.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `initial` | number | the reference price | Starting value, and the level it reverts to |
| `mean_reversion` | number | `0` | Per second; 0 makes the value a random walk |
| `volatility` | number | `1` | Ticks per square root of a second |
| `step` | duration | `"100ms"` | How often the value changes |
| `jump_rate` | number | `0` | News per second: jumps come at random times, this many a second on average |
| `jump_size` | number | `0` | Ticks: the standard deviation of each jump, which is normally distributed |

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

## `[scoring]` (optional)

How each seat of a played or served session is scored; [design.md](design.md#scoring-and-challenges)
explains the score. Agents' runs ignore it. Amounts are in ticks, with at most three decimals;
scores are reported in tick-lots.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `mark` | text | `"last"` | What positions are valued at: `"last"`, the last trade price, or `"value"`, the true value at the end, which needs a `[fundamental]` section |
| `inventory_penalty` | number | 0 | Charged per lot held per second |
| `close_penalty` | number | 0 | Charged per lot still held at the end |
| `max_loss` | whole number | 0 | A loss this large, in tick-lots, valued at the latest trade, stops the seat: its orders are cancelled and it can send no more. 0 for no limit |

### `[scoring.target]` (optional)

A large order to work. The score is measured against a paper portfolio that traded the whole
target at the benchmark price.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `side` | text | required | `"buy"` or `"sell"` |
| `quantity` | whole number | required | Lots |
| `benchmark` | text | `"vwap"` | `"vwap"`, the session's volume-weighted average price, or `"reference"`, the reference price |
| `unfinished_penalty` | number | 0 | Charged per lot of the target not done |

## `[challenge]` (optional)

Makes the scenario a challenge: `play`, `connect` and every client of `serve` are shown its name
and briefing before the clock starts.

| Key | Type | Meaning |
|---|---|---|
| `name` | text | The challenge's name |
| `briefing` | text | What the participant is asked to do, and how it is scored |

[examples/challenges](../examples/challenges) has four: making markets within a risk limit,
working a large order, trading the news, and making markets against informed traders.

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

The market maker receives every trade and quote change as it happens. The others read the market
on demand when they act, which keeps crowds of thousands fast; either way, each sees the market
only after its own `from_exchange` latency. Agents that act on a timer (the market maker and the
momentum, informed and adaptive traders) start it at a random point within the first interval, and
execution agents wait a random pause before their first parent, so a group of them does not act in
lockstep.

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
| `activity_response` | `0` | How the trader's pace follows the market's: its rates are multiplied by (recent trade rate ÷ usual trade rate) to this power, within 0.1 to 10 |
| `volatility_response` | `0` | How its limit orders stand back when prices jump: `max_offset` is multiplied by (recent volatility ÷ usual volatility) to this power, within 0.25 to 4 |
| `activity_memory` | `"60s"` | How far back "recent" reaches, for both responses |
| `activity_baseline` | `"1800s"` | How far back "usual" reaches; no shorter than `activity_memory` |

The trader measures activity from the trades published so far and volatility from the moves of
the mid, both as it sees them when it acts. With the responses at 0, the default, it behaves as it
always has. [memory_market.toml](../examples/scenarios/memory_market.toml) uses both.

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

### `adaptive`

Switches between a value strategy and a trend strategy by how well each has been doing, after
Brock and Hommes (1998). Needs a `[fundamental]` section.

Every interval it looks at the price and, with noise, at the true value. The value strategy calls a
buy when the value is above the price, by at least `threshold`, and a sell when it is below by as
much; the trend strategy calls a buy when a fast moving average of the price leads a slow one, by at
least `threshold`, and a sell when it trails by as much. Each keeps a track record: the price change
after each of its calls, in the call's direction, fading with half-life `memory`. The trader follows
the trend strategy with probability
1 ÷ (1 + e^(−`choice_intensity` × (trend record − value record))), and its position follows the
chosen strategy's call: long `max_position` on a buy, short on a sell, flat without a call. It
trades toward that position at the market, `order_size` at a time.

| Parameter | Default | Meaning |
|---|---|---|
| `interval` | `"1s"` | How often it decides |
| `noise` | `2.0` | Ticks; standard deviation of each look at the value |
| `fast_half_life`, `slow_half_life` | `"2s"`, `"20s"` | The trend strategy's moving averages |
| `memory` | `"60s"` | Half-life of the track records |
| `choice_intensity` | `1.0` | Per tick of track record: how surely the better strategy is chosen |
| `threshold` | `1.0` | Ticks of signal before a strategy calls a trade |
| `order_size` | `2` | Lots per order |
| `max_position` | `20` | The position it holds when its strategy calls a trade |

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

### `execution`

A broker's algorithm working large orders, one at a time. After a pause, exponential with mean
`pause`, it takes a parent order of a random side and a size from a Pareto tail,
P(size > q) = (`min_parent` / q)^`parent_tail`, capped at `max_parent`, and trades it with child
market orders: `child_size` lots every `interval` for `twap`; for `pov`, every `interval`, enough to
keep its own trading at `participation` of all the volume traded since the parent started, up to
`child_size` at a time. Lots a child leaves unfilled are sent again; if the exchange rejects a
child, as at the account's position limit, it gives up on the rest of the parent. Every child
carries the parent's id, 1, 2, 3, ... for each agent, in the `parent` column of the event log.

Positions follow the random sides of the parents, so give execution agents a large `max_position`
in their account. [large_orders.toml](../examples/scenarios/large_orders.toml) has a hundred of
them.

| Parameter | Default | Meaning |
|---|---|---|
| `style` | `"twap"` | `"twap"` for an even pace in time, `"pov"` for a share of the volume |
| `min_parent` | `20` | Lots; the smallest parent |
| `parent_tail` | `1.5` | Exponent of the Pareto tail of parent sizes |
| `max_parent` | `5000` | Lots; parent sizes are capped here |
| `pause` | `"300s"` | Mean wait before each parent |
| `interval` | `"1s"` | Between child orders |
| `child_size` | `5` | Lots per child; for `pov`, the most per child |
| `participation` | `0.1` | For `pov`: its share of all the volume while it works a parent |

## Your own agents

Subclass `crowdbook::Agent`, register a factory for it under a name, and use that name as the
`type` of an `[[agents]]` table. [examples/custom_agent.cpp](../examples/custom_agent.cpp) does this
in about 40 lines; the factory reads its parameters from `crowdbook::Parameters`, and anything it
does not read is reported as an unknown parameter. An agent that only needs the market when it
acts should override `marketData()` to return `MarketDataMode::Snapshot` and call
`context.market()`, instead of receiving every update. Snapshots also carry the trades and the
volume published so far, which is how an agent can tell how busy the market has been.

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
| `parent` | For `new`: the sender's id for the larger order this one is part of, if it gave one |

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

A played, served or replayed session of a scenario with `[scoring]` also has `scores`, one per
seat: `seat`, then `total`, `pnl`, `inventory`, `close`, `paper` and `unfinished` in points
(thousandths of a tick-lot), `unfinished_lots`, and `stopped_at_ns`, when the loss limit stopped
the seat, or `null`.

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
| `session_version` | `2` |
| `seed` | The seed the session ran with |
| `end_ns` | When it stopped, in simulated nanoseconds |
| `seats` | The names of the session's seats, in the order their participants joined: `["you"]` for `play` |
| `scenario` | The scenario file's text, so a session replays even if the file changes |
| `actions` | The participants' requests in time order, one table each: `time_ns`, `seat` (an index into `seats`), `instrument` (always 0 for now), `request` (`new`, `cancel` or `modify`) and the request's fields: `client_order_id`, `side`, `type`, `time_in_force`, `price`, `quantity` |

Version 1 files, from before sessions could have several seats, have no `seats` and no `seat` in
their actions; they still replay, with the one seat `you`.

A replay checks that each new order gets the client order id the session recorded, and stops with
an error if it does not, which would mean the market had come out differently.

## Serving and connecting

```bash
./build/release/apps/crowdbook serve examples/scenarios/playable.toml --seat alice --seat bob \
    --record session.toml
./build/release/apps/crowdbook connect 127.0.0.1:7878 --seat alice
```

`serve` runs the scenario in real time for clients that connect over TCP and speak the protocol in
[protocol.md](protocol.md). Each seat is a participant with the scenario's `[participant]` account
and latency, reported as a group named after the seat. The clock starts once every seat is
claimed, and the session ends at the scenario's duration or on ctrl-c. It takes `--seed`,
`--duration`, `--speed`, `--record`, `--log`, `--log-only` and `--json` like `play` and `run`, and:

| Option | Meaning |
|---|---|
| `--seat NAME` | A seat, named by 1 to 32 letters, digits, `-` or `_`; repeat for more. One seat, `you`, if none is named |
| `--listen HOST:PORT` | Where to listen; `127.0.0.1:7878` unless given. `:7878` listens on every address, and port 0 picks a free one |
| `--tokens FILE` | Seats' tokens, one `seat token` pair per line; a client must give its seat's token. Every seat needs one |
| `--rate-limit N` | Messages each connection may send per second; 500 unless given |

A recorded session replays with `replay` like one from `play`, every seat's orders included.

`connect` is the trading screen of `play` for one seat of a served market, with the same keys
except that speed and pause belong to the server. It takes `--seat`, `you` unless given, and
`--token`.
