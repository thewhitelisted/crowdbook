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

Durations are written as text with a unit: `"250ns"`, `"50us"`, `"1.5ms"`, `"2s"`, `"90m"`,
`"1.5h"`, and are shorter
than `"1000000000s"`, about 31 years. Numbers must be finite: `nan` and `inf` are errors. Prices are
in ticks and quantities in lots. Any key crowdbook does not know is an error, so a typo stops the
run instead of silently using a default.

## Top level

| Key | Type | Default | Meaning |
|---|---|---|---|
| `seed` | integer | `1` | The run's seed; the same seed always gives the same run |
| `duration` | duration | `"60s"` | How much simulated time to run |
| `reference_price` | integer | `10000`, or in a prediction market the starting probability in cents | Where agents anchor before they have seen any quotes |

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
| `path` | list of pairs | none | A recorded value instead of the walk: `[[seconds, ticks], ...]`, in order of time, from the market's opening. The value is `initial` until the first point and each point's from its time until the next, the later of two at one time; it takes no `mean_reversion`, `volatility`, `jump_rate` or `jump_size` |

With a `path`, the true value is one that was recorded, such as an index's price for a perpetual
future, so that a market can be made to see the moves a real one saw. It holds the last point's
value to the end: there is nothing to settle. `crowdbook-calibrate` fills `${value_path}` and
`${opening_value}` with a recorded market's, in ticks, for a tape that has a value table and no
payout.

## `[prediction]` (optional)

Makes the market a prediction market on one yes-or-no question, the kind Polymarket and Kalshi
run, which resolves at the end of the run. A share pays 100 if the answer is yes and nothing if it
is no, so prices are cents from 1 to 99 and the exchange takes none higher. The true value is the
probability of yes, in cents: the chance, given everything so far, that a hidden quantity which
wanders and jumps with news ends above zero. It moves slowly while much is unknown and faster as
resolution nears, and at the end it is 100 or 0. Informed and adaptive agents observe it with
noise; every agent keeps its prices within 1 to 99. It takes no `[fundamental]` section.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `probability` | number | `0.5` | The chance of yes at the start, between 0 and 1 |
| `news_rate` | number | `0` | News per second: pieces of news come at random times, this many a second on average |
| `news_share` | number | `0` | How much of the question's uncertainty arrives as news, from 0 to below 1; the rest comes a little at a time. Needs `news_rate` |
| `step` | duration | `"1s"` | How often the hidden quantity moves |
| `path` | list of pairs | none | A recorded value instead of the hidden quantity's: `[[seconds, cents], ...]`, in order of time, from the market's opening, cents from 0 to 100 |
| `outcome` | number | none | With a `path`, the value at resolution, in cents from 0 to 100 |

At most 500 pieces of news may be expected over the run. The results value each group's position
at resolution, beside the last price.

With a `path`, the true value is not a model's but one that was recorded, such as the price of a
sharper market on the same question, so that the market's informed traders and makers see the
moves a real one saw. It is `probability` until the path's first point, each point's from its time
until the next (the later of two at one time), and `outcome` at resolution, or the last point's
where there is no outcome. It takes no news and no game. `crowdbook-calibrate` fills `${value_path}`
and `${outcome}` from a tape's `value` table and outcomes for a twin of a recorded market:

```toml
[prediction]
probability = ${opening_probability}
path = ${value_path}
outcome = ${outcome}
```

### `[prediction.game]` (optional)

Makes the question a game between a home and an away side, on a clock: will the home side win?
Before kickoff the sides' relative strength drifts as information arrives, now and then in a jump
of news; during play each side scores at random, at a rate its strength sets, and the strength can
drift too, while the clock stops between periods. The true value is the probability that the home
side ends ahead, given the score, the playing time left and the strength as it is now, worked out
exactly from the distribution of the points each side has still to score; it jumps on every
score, and once the game ends it is 100, 0, or what a tie pays, and stays there until the market
settles at the end of the run. `probability` is the home side's chance at kickoff, and the
strength starts wherever gives it; `news_rate` and `news_share` are not used with a game, and
`step` is how often the game moves. [game.toml](../examples/scenarios/game.toml) is one. A contract on
a total or a spread is priced the same way, from the points still to come; a player's prop, over
or under a number of that player's points, is a total of a game whose scoring is the player's
alone.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `side` | string | `"home"` | What the contract pays 100 for: `"home"`, a win for the home side, `"away"`, a win for the away side, or `"draw"`; `"over"` or `"under"`, both sides' points together over or under `line`; `"home_spread"` or `"away_spread"`, the home side's lead over or under `line`. `probability` is its price at kickoff: for a win, a draw or a spread it sets the sides' strength, the home side taken to be the stronger for a draw; for a total it sets how much is expected to be scored, `scores` scaled to give it |
| `line` | number | `0` | The line of a total or a spread, in points; a half point never ties, and a result exactly on a whole one pays `tie` |
| `start` | duration | `"0s"` | Kickoff, after the market opens |
| `length` | duration | `"90m"` | Playing time, all periods together |
| `periods` | integer | `1` | Periods of equal playing time, from 1 to 100 |
| `break` | duration | `"0s"` | How long the clock stops between two periods |
| `scores` | number | `2.5` | Scoring events expected in the game, both sides together; more than 0, at most 500 |
| `late_scoring` | number | `1` | How many times faster scoring events come at the end of the playing time than at its start, the rate changing in a straight line between; above 0, at most 100 |
| `points` | list of integers | `[1]` | What a scoring event can be worth, from 1 to 100 |
| `point_weights` | list of numbers | equal | How often each of `points`, in proportion |
| `tie` | number | `0` | Cents a share pays if the game ends level, for a win, or exactly on the line, for a total or a spread; from 0 to 100 |
| `pregame_drift` | number | `0` | The standard deviation of the strength's change from the opening to kickoff |
| `in_play_drift` | number | `0` | The standard deviation of its change over the playing time |
| `news` | number | `0` | Pieces of news expected before kickoff, at most 500 |
| `news_size` | number | `0` | The standard deviation of each piece's move in the strength |
| `script` | list of tables | none | The scores of a game that was played, in order, instead of random ones: `{ at = "47m", side = "home", points = 1 }`, `at` from the market's opening; `points` is 1 unless given |

The strength is in log-odds of who scores: at strength s the home side scores a share
1 ÷ (1 + e^(−s)) of the scoring events, so 0 is an even game and ±1 makes one side score 73% of
them. For a total, which does not see who scores, the strength scales how much is scored
instead, by e^s, starting at 0: the drift and the news move what a total is expected to make, each
move giving up half its variance so that e^s keeps its mean. Either way the value is the average
of the price over where the strength may still go, with the variance it has left, so that it
stays a fair price however much it drifts. `line` goes only with a total or a spread. A total so
high that the chance of no score underflows (over 600 scoring events to come) takes its points to
come as normal. A run that ends before the game does, after its periods and breaks, stops with the game
unfinished, its value the probability then. With a `script` the score follows it, at the times
given even outside the clock, as in time added on, while the strength still drifts and the value
still prices what is to come from the expected scoring: one market can be made to replay a game
that was played. A `probability` that
no strength reaches is an error, as in a low-scoring game where a tie pays nothing and the home
side cannot be made a near-certainty.

## `[exchange]` (optional)

Rules and market data that apply to everyone.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `depth_levels` | integer | `0` | Price levels per side in the public depth feed, up to 1,000; 0 publishes only the best bid and ask |
| `maker_fee` | number | `0` | Ticks per lot paid by the owner of the resting order in every trade; negative is a rebate |
| `taker_fee` | number | `0` | Ticks per lot paid by the owner of the incoming order |
| `auction_fee` | number | `0` | Ticks per lot paid by both sides of a trade in an auction's uncross; not negative |
| `fee_curve` | string | `"flat"` | How fees vary with the price of a fill: `"flat"`, or `"probability"` in a prediction market, where a fill at price p pays each fee times 4·q·(1 − q) for q = p ÷ 100, the share of the payout: the whole fee at 50 and less toward either end |
| `order_delay` | duration | none | New orders and amendments wait this long at the exchange before they reach the book, as betting exchanges hold bets while an event is in play; cancels do not wait, so a resting order can still be pulled at once. An order is accepted, or rejected, only when it reaches the book |
| `clear_at` | list of durations | none | Times at which the exchange clears its book, cancelling every resting order, as betting exchanges do when an event goes in play; each owner hears its orders cancelled with the reason `cleared` |
| `order_delay_cancels` | boolean | `false` | Hold cancels too, as long as orders; with `false` they go straight through. Needs an `order_delay` |
| `order_delay_from`, `order_delay_until` | duration | the start, the end | The stretch of the run in which orders are held: an order is held if it reaches the exchange from `order_delay_from` until before `order_delay_until`. They need an `order_delay` |

Fees take at most three decimals, such as `-0.25`, and `maker_fee + taker_fee` must not be
negative: no exchange pays out more in rebates than it collects. Fees are kept apart from cash,
which only trades move, and every fill reports its own fee.

Polymarket charges takers about 0.07·q·(1 − q) dollars a share for a share priced at q dollars,
1.75 cents at 50; `taker_fee = 1.75` with `fee_curve = "probability"` charges the same, in cents.
Under the curve each fill's fee per lot is rounded to the nearest thousandth of a tick, so it is
the same on every platform.

A depth feed lets agents see the book beyond the best prices, both in `context.market()` and, for
agents that stream market data, in `onDepth`. It costs time: ten levels add about half again to
the run time of the thousand-trader example market, so it is off unless a scenario asks for it.

## `[trading_day]` (optional)

Makes the market trade the way a stock exchange's day goes: an opening auction, continuous
trading, a closing auction, then closed, with halts. [design.md](design.md#trading-day) explains
the rules; without this section the market trades continuously from start to end.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `opening_auction` | duration | `"0s"` | A call auction from the start, uncrossed when it ends; none if 0 |
| `closing_auction` | duration | `"0s"` | A call auction before the end, uncrossed at the end; none if 0 |
| `halt_band` | integer | `0` | Ticks: a continuous trade further than this from the last auction's price, or the reference price before any, halts the market; 0 never halts |
| `halt` | duration | none | How long a halt's auction lasts; needed with a `halt_band` |
| `activity` | number | `0` | The depth of a U-shaped curve that the noise traders' pace and VWAP brokers follow: (1 + a·u) / (1 + a/3), where u runs from 1 at the start to 0 at midday and back to 1 at the end. 0 is flat |

In an auction, limit orders rest without trading and the book may cross; market,
immediate-or-cancel and post-only orders are rejected. After the close only cancels are taken.
The opening and closing auctions together must fit in the duration. `crowdbook play` and `serve`
end a trading day once the close's messages have reached the participants, a latency after the
duration.

## `[participant]` (optional)

The account and latency of the person trading in the market with `crowdbook play`. Agents' runs
ignore it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `latency` | table | no delay | `to_exchange`, `from_exchange` and `jitter`, all durations; either of the first two may be a distribution ([below](#latencies-drawn-from-a-distribution)) |
| `account` | table | no limits | `initial_cash`, `initial_position`, `max_position`, `max_order_quantity` |

### Latencies drawn from a distribution

A link's delay can be a measured distribution instead of one duration: `{ quantiles = [...] }`,
durations shortest first, evenly spaced in probability from the shortest to the longest, as many as
the measurement supports. Each message on that link takes a delay drawn afresh: one of the gaps
between neighbouring quantiles, each as likely, then uniformly within it. `jitter` still adds to
it. Messages on a link still arrive in the order they were sent, so one that draws a long delay
holds up those behind it, as on one connection.

```toml
[participant.latency]
# 11 quantiles, from the 0th percentile to the 100th by tens: a delay with a long tail.
to_exchange = { quantiles = ["8ms", "9ms", "9ms", "10ms", "10ms", "11ms", "12ms", "14ms", "18ms",
                             "30ms", "250ms"] }
from_exchange = "10ms"
```

Where one number stands for the link, its median does: an agent that reads the market on demand
sees it that late, and a served seat's `welcome` reports it.

## `[scoring]` (optional)

How each seat of a played or served session is scored; [design.md](design.md#scoring-and-challenges)
explains the score. Agents' runs ignore it. Amounts are in ticks, with at most three decimals;
scores are reported in tick-lots.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `mark` | text | `"last"` | What positions are valued at: `"last"`, the last trade price; `"value"`, the true value at the end, which needs a `[fundamental]` section; or `"book"`, what closing the position through the book would bring, taking the book best price first and paying the taker fee, with the seat's own orders left out and lots beyond the book at its deepest price, or at the last trade when that side holds nothing but the seat's own orders |
| `inventory_penalty` | number | 0 | Charged per lot held per second |
| `close_penalty` | number | 0 | Charged per lot still held at the end |
| `max_loss` | whole number | 0 | A loss this large, in tick-lots, valued at the latest trade, stops the seat: its orders are cancelled and it can send no more. 0 for no limit |

### `[scoring.target]` (optional)

A large order to work. The score is measured against a paper portfolio that traded the whole target
at the benchmark price. With `mark = "book"`, the paper portfolio closes through the book only as
much of the target as the seat has done, and values the rest at the last trade.

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

## `[learning]` (optional)

How a learning agent steps the market from Python or C++ through one seat, `learner`, which has the
`[participant]` account and latency and is scored by `[scoring]`;
[design.md](design.md#learning-environment) describes the environment. Runs, plays and served
sessions ignore it. A scenario without it can still be stepped, with every setting at its default.

```python
import crowdbook_learn
pool = crowdbook_learn.Pool("examples/learning/market.toml", markets=64)
obs, info = pool.reset(seeds=range(64))
obs, rewards, terminated, truncated, info = pool.step(actions)  # 64 rows of (action, size)
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `interval` | duration | `"1s"` | Decisions come at every multiple of it, and whenever one of the seat's orders fills |
| `warmup` | duration | `"1s"` | The first decision; before the end of the run |
| `actions` | list of text | all sixteen | The actions the seat can take, numbered in this order |
| `sizes` | list of whole numbers | `[1, 5]` | The sizes an action can be taken at, in lots, numbered in this order |
| `take_ticks` | whole number | 0 | How far past the other side's best price `buy`, `sell` and `close` will trade |
| `reward_mark` | text | `"value"` with a `[fundamental]` or `[prediction]` section, else `"book"` | What positions are valued at between decisions: `"last"`, `"value"` or `"book"`, as `mark` in `[scoring]` |
| `close` | text | `"value"` in a prediction market, else `"book"` | What positions are valued at when the episode ends: the same choices |
| `max_events` | whole number | 256 | Rows of the event stream; the latest are kept |
| `max_depth_updates` | whole number | 32 | Depth updates kept with their levels; 0 for none |
| `depth_levels` | whole number | 10 | Levels a side of each depth update and of the latest book; 0 for none. The exchange's `depth_levels` sets how many it sends |
| `max_orders` | whole number | 8 | Rows of the seat's open orders |
| `feed` | table | none | A view of the underlying, which needs a `[fundamental]` or `[prediction]` section: `interval`, how often it comes, `"1s"`, at least `"1ms"`; `lag`, how late, 0; `noise`, the standard deviation of the normal noise added to each, 0; `trigger`, how far a publication must move from the one heard at the last decision to be a decision of its own, 0 for never; `of`, `"underlying"`, the default, or `"value"`, the true value itself, in ticks or, in a prediction market, cents of the payout, as a sharper market's price elsewhere would be |
| `ranges` | table | none | Settings drawn for each episode ([ranges](#learningranges-optional)) |

The actions, each taken at one of the sizes:

| Action | What the seat does |
|---|---|
| `hold` | Nothing |
| `bid_join`, `ask_join`, `both_join` | Bids, offers, or both, at the best price on that side |
| `bid_improve`, `ask_improve`, `both_improve` | A tick better than the best price on that side, but never at or through the other side; where the spread leaves no room for both, `both_improve` joins both |
| `bid_behind`, `ask_behind`, `both_behind` | A tick behind the best price on that side |
| `cancel_bid`, `cancel_ask`, `cancel_all` | Cancels its bid, its offer, or both |
| `buy`, `sell` | Takes the other side at its best price, `take_ticks` past it, immediate-or-cancel, having cancelled its own order on that side |
| `close` | Cancels both and takes its position off the same way, as much as one order of `max_order_quantity` may; a larger position needs another `close` |

The best prices are those the seat has heard, its own order left out: where its order is alone at
the best price, the next level of the depth it heard. The seat keeps at most one order a side,
post-only; quoting a side again moves that order to the new price with a modify, or leaves it
where it is in the queue if the price is the same. Sizes are cut to what the position limit and
`max_order_quantity` allow. An action that needs a price the seat has not heard does nothing.

The observation, in whole numbers: `state`, the seat's time, time left, position, cash, fees, open
quantity, the best prices, the last trade, the phase, the reference and highest prices, its position
and order limits, and how many requests were rejected; `events`, everything heard since the last
decision, a row `[type, time, a, b, c, d, e]` each, the latest `max_events` kept; `depth`, the
levels of the depth updates in the stream, which their rows point into; `book`, the latest depth
heard; `orders`, the seat's open orders; and `counts`, how many events, depth updates and orders
there were in all. `STATE_NAMES` and `EVENT_NAMES` in the Python module name the columns and types,
and `include/crowdbook/learning.hpp` describes each field. The reward is the change in the seat's
score since the last decision, in tick-lots, the first counting from the start, so that an episode's
rewards add up to its score; the score as the episode would end now and the true value come beside
it, in `info`, never in the observation.

### `[learning.ranges]` (optional)

Settings drawn at random for each episode, from a seed of its own. Each key is a setting's place in
the scenario file, where `agents.<group>` names an agent group by its `name`, or by its `type` if it
has no name; each value is how it is drawn:

| Form | Draws |
|---|---|
| `[low, high]` | Uniformly between the ends: whole numbers, numbers, or durations, with both ends included for whole numbers and durations |
| `{ range = [low, high], step = s }` | One of low, low + s, low + 2s, ... up to high, each as likely |
| `{ log = [low, high] }` | Uniformly in the logarithm, between two positive numbers |
| `{ choices = [a, b, ...] }` | One of the choices, each as likely: numbers, text, or true or false |

```toml
[learning.ranges]
duration = ["300s", "900s"]
fundamental.volatility = { log = [0.25, 1.0] }
exchange.taker_fee = { range = [0.1, 0.3], step = 0.05 }
agents.noise.count = [40, 80]
agents.followers.lag = ["10s", "60s"]
```

Each range draws from its own random stream, keyed by the episode's settings seed and the range's
key, so the same seed always draws the same settings and adding a range changes no other's draw. The
drawn values are set in the scenario and read with every check a scenario file gets. So that a range
no market can be built from fails at once rather than in an episode, the scenario is built when it
is loaded with every range at its low end, at the highest value it draws, at each choice and at
sixteen draws. Settings whose values must be multiples of something, such as fees in thousandths of
a tick, want a step. `info["settings"]` reports each episode's values, a choice of text by its
position in the list.

[examples/learning](../examples/learning) has a market to step, with its settings drawn for each
episode.

## `[[agents]]`

Each `[[agents]]` table is a group of agents of one type that share their settings. Every agent
still gets its own id and its own random streams.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | text | required | A built-in type below, or one you register |
| `name` | text | the type | Label in the results |
| `count` | integer | `1` | How many agents the group has; 0 leaves the group out, its settings still checked, as a template switches a group off for some markets |
| `start` | duration | the start of the run | When the agents start |
| `latency` | table | no delay | `to_exchange`, `from_exchange` and `jitter`, all durations; either of the first two may be a distribution ([below](#latencies-drawn-from-a-distribution)) |
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
| `market_protection` | `0` | A worst price for market orders, in ticks beyond the last trade price the trader saw (the mid before any trade): the order takes what the book has up to there and the rest is cancelled, as exchanges' price protection and Polymarket's market orders do. `0` sends true market orders, which take the book at any price |
| `size_tail` | `0` | `0` draws sizes uniformly from the range. A positive value draws them from a Pareto tail, P(size > q) = (`min_size` / q)^`size_tail`, capped at `max_size`: most orders small and a few far larger, as in real markets |
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
`γσ²τ/2 + ln(1 + γ/k)/γ`. Its fair price `s` is a running average of trade prices, or, with
`quote_value`, the true value as it sees it at each requote: a market maker who prices off what the
asset is worth. That needs a `[fundamental]` or `[prediction]` section.

| Parameter | Default | Meaning |
|---|---|---|
| `risk_aversion` | `0.005` | γ: how hard inventory skews the quotes |
| `volatility` | `3.0` | σ: ticks per square root of a second |
| `intensity` | `0.3` | k: the base half spread is about 1/k ticks |
| `horizon` | `"1s"` | τ: the holding period inventory risk is priced over |
| `quote_size` | `5` | Lots per quote |
| `max_inventory` | `50` | Never quotes a side that could take its position past this |
| `requote_interval` | `"100ms"` | How often it requotes; it also requotes after each of its fills |
| `requote_threshold` | `0` | Ticks: at a requote its timer brings, it leaves its quotes where they are while its fair price is less than this from where it was when it last set them and every quote it set then is still in the book at its full size, as makers who do not chase each small move of a reference do; a fill or a rejected modify requotes it regardless, and a side it left out then, at its inventory limit or where a post-only quote would have met the book, stays out until it requotes |
| `fair_value_weight` | `0.2` | How far each trade moves its fair price toward the trade's price |
| `flow_impact` | `0` | Ticks each lot of aggressive volume in a trade it sees moves its fair price, up for a buy and down for a sell, on top of the price it anchors on (the value with `quote_value`, else the average of trades), as a dealer reads order flow in Glosten and Milgrom (1985) and Kyle (1985); an auction's trades move it not at all. `0` reads no flow |
| `flow_memory` | none | How long that impact lasts: it decays by a factor of e every `flow_memory`. None keeps it for good, as flow from traders who know the value; a finite memory makes the mid revert |
| `take_through` | none | Ticks: at a requote, when its fair price is past the far side of the book by more than this and the book shows someone besides itself there, it sends an immediate-or-cancel order against the stale quotes there, at its fair price less this, for as much as `max_inventory` leaves room for counting every order it has open, then quotes as if that filled. It first cancels its own quotes the take would reach, which the exchange would otherwise stop it at; its own takes' trades move its fair price not at all; and when a take's rest is cancelled it quotes afresh, taking nothing more until its timer. A take is not post-only even with `post_only`. An account `max_position` below `max_inventory` gets such a take refused, as any order would be. None never takes |
| `post_only` | `false` | Quote with post-only orders, a tick off the far side of the book it last saw, so it only ever adds liquidity |
| `quote_value` | `false` | Quote around the true value as it sees it, rather than the average of trades |
| `noise`, `error`, `error_memory`, `bias`, `tilt`, `lag` | `1.0`, `0`, `"60s"`, `0`, `0`, `"0s"` | With `quote_value`, how it sees the value, as [below](#how-traders-see-the-true-value); without it, not allowed |
| `quiet_from`, `quiet_until` | none, the end | A quiet stretch of the run, as makers who stay away from an event in play: from `quiet_from` until `quiet_until` |
| `quiet_interval` | `"0s"` | In the quiet stretch: with `"0s"` it pulls its quotes and stays out; otherwise it requotes only this often, leaving its quotes where they are between |
| `levels` | `1` | A ladder of this many quotes a side instead of one, up to 50: the first where the one quote would be, each further one behind it |
| `level_spacing` | `1` | Ticks between one level of the ladder and the next |
| `level_growth` | `1.0` | Each level's size over the size of the level nearer the touch: above 1, the book is deeper further out, as real books often are. A level is quoted only while filling it and every level nearer would keep the position within `max_inventory` |

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
Brock and Hommes (1998). Needs a `[fundamental]` or `[prediction]` section.

Every interval it looks at the price and, as it sees it ([how traders see the true
value](#how-traders-see-the-true-value)), at the true value. The value strategy calls a
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
| `noise`, `error`, `error_memory`, `bias`, `tilt`, `lag` | `2.0`, `0`, `"60s"`, `0`, `0`, `"0s"` | How it sees the value; see below |
| `fast_half_life`, `slow_half_life` | `"2s"`, `"20s"` | The trend strategy's moving averages |
| `memory` | `"60s"` | Half-life of the track records |
| `choice_intensity` | `1.0` | Per tick of track record: how surely the better strategy is chosen |
| `threshold` | `1.0` | Ticks of signal before a strategy calls a trade |
| `order_size` | `2` | Lots per order |
| `max_position` | `20` | The position it holds when its strategy calls a trade |

### `informed`

Follows the true value, as it sees it, and trades when the book is outside its range of fair
values, using immediate-or-cancel orders priced to keep its edge. Needs a `[fundamental]` or
`[prediction]` section.

To hold the price to the value over a long run, the traders who follow it must absorb the other
traders' net order flow. Give them room the way real markets do, with a crowd of traders on
ordinary limits rather than a few with huge ones: with too little room in all, they fill up and the
price drifts away from the value. [large_market.toml](../examples/scenarios/large_market.toml)
shows a crowd that holds for a simulated day. Mind its `threshold` too: a large crowd with narrow
ranges, spread around the value by their errors, stands ready at every price near it, a deep book
of its own that can smother effects that need the book to thin
([results.md](results.md#memory-what-makes-volatility-cluster)).

| Parameter | Default | Meaning |
|---|---|---|
| `interval` | `"100ms"` | How often it looks at the value |
| `random_interval` | `false` | `true` waits an exponential draw of mean `interval` between looks, the first included, instead of `interval` itself: its looks come as a Poisson process, rare and bunched as a few large traders' decisions are, rather than like clockwork |
| `noise`, `error`, `error_memory`, `bias`, `tilt`, `lag` | `1.0`, `0`, `"60s"`, `0`, `0`, `"0s"` | How it sees the value; see below |
| `threshold` | `3.0` | Ticks of edge needed before trading: half the width of its range of fair values, inside which it does nothing |
| `order_size` | `5` | Lots per order |
| `max_position` | `50` | Largest position it builds, counting orders in flight |
| `size_tail` | `0` | `0` sends every order at `order_size`. A positive value draws each order's size from a Pareto tail, P(size > q) = (`order_size` / q)^`size_tail`, cut to the room `max_position` leaves |

### How traders see the true value

Nobody knows the true value exactly. Informed and adaptive traders, and market makers that quote
off the value, see it through a belief, set by six parameters, in ticks (cents in a prediction market):

| Parameter | Meaning |
|---|---|
| `noise` | Standard deviation of a fresh error on every look |
| `error` | Standard deviation of the trader's own lasting error, which wanders slowly; 0 for none |
| `error_memory` | How long a lasting error lasts: after this long, what it was and what it is have a correlation of 1/e |
| `bias` | Added to every look: the group's leaning, such as partisans who want the answer to be yes |
| `tilt` | In a prediction market, how far the trader pulls probabilities toward even, in log-odds: it sees logistic((1 − `tilt`) · logit(p)) for a probability p. Above 0 it rates long shots too high and favourites too low, the favourite–longshot bias of bettors; below 0, the reverse. Below 1; `0` by default |
| `lag` | How late, on average, the trader sees the value. Each trader of a group gets its own delay, between none and twice this, so some hear the news first and some last |

With the defaults a trader sees the value as it is now with fresh noise alone. The examples mix a
few experts, quick and close to right, with a larger crowd of followers who see the value half a
minute or more late with views of their own, and in the prediction markets partisans, so that the
price earns its accuracy instead of having it by construction.

### `probe`

A small, simple strategy that measures a market rather than moves it: a maker at a set width, a
trend follower, a reversal trader or a flow follower. Run alone and small in a market fitted to a
real one, any that makes money after fees is an edge the fitted market offers; `crowdbook-sweep`
runs them ([validation.md](validation.md#the-no-false-edge-check)). Takers take only at the touch,
with immediate-or-cancel orders, and a probe keeps its position within `max_position` even if all
its orders in flight fill.

| Parameter | Default | Meaning |
|---|---|---|
| `style` | `"maker"` | `"maker"` quotes a bid and an ask `offset` ticks either side of the mid, rounded away from it, requoting a side when its price moves; `"trend"` takes toward its limit when a fast moving average of the mid leads a slow one by `threshold`, the other way when it trails; `"reversal"` the opposite; `"flow"` takes toward its limit on the side that took at least `imbalance` of the volume traded in the last `window` |
| `interval` | `"1s"` | How often it looks |
| `size` | `1` | Lots an order |
| `max_position` | `10` | The largest position it builds |
| `offset` | `1.0` | A maker's half width, in ticks from the mid |
| `fast_half_life`, `slow_half_life` | `"10s"`, `"60s"` | A trend's or a reversal's moving averages |
| `threshold` | `1.0` | Ticks the fast average must lead the slow one by |
| `window` | `"20s"` | The recent volume a flow follower weighs |
| `imbalance` | `0.6` | Its share on one side before it follows, from 0.5 to below 1 |

### `execution`

A broker's algorithm working large orders, one at a time. After a pause, exponential with mean
`pause`, it takes a parent order of a random side and a size from a Pareto tail,
P(size > q) = (`min_parent` / q)^`parent_tail`, capped at `max_parent`, and trades it with child
market orders: `child_size` lots every `interval` for `twap`; for `pov`, every `interval`, enough to
keep its own trading at `participation` of all the volume traded since the parent started, up to
`child_size` at a time; for `vwap`, every `interval`, `child_size` scaled by the trading day's
activity curve at that moment. Lots a child leaves unfilled are sent again. In an auction it sends
nothing, and a child the exchange turns away because an auction began, or the market closed,
while it was on its way is sent again later; if the exchange rejects a child for any other reason,
as at the account's position limit, it gives up on the rest of the parent. Every child
carries the parent's id, 1, 2, 3, ... for each agent, in the `parent` column of the event log.

Positions follow the random sides of the parents, so give execution agents a large `max_position`
in their account. [large_orders.toml](../examples/scenarios/large_orders.toml) has a hundred of
them.

| Parameter | Default | Meaning |
|---|---|---|
| `style` | `"twap"` | `"twap"` for an even pace in time, `"pov"` for a share of the volume, `"vwap"` for the day's activity curve |
| `min_parent` | `20` | Lots; the smallest parent |
| `parent_tail` | `1.5` | Exponent of the Pareto tail of parent sizes |
| `max_parent` | `5000` | Lots; parent sizes are capped here |
| `pause` | `"300s"` | Mean wait before each parent |
| `interval` | `"1s"` | Between child orders |
| `child_size` | `5` | Lots per child; for `pov`, the most per child |
| `participation` | `0.1` | For `pov`: its share of all the volume while it works a parent |
| `interval_spread` | `0` | How far each wait between children strays from `interval`: drawn uniformly within this share either side, so `0.5` waits from half an interval to one and a half. From 0 up to, but not including, 1 |
| `size_spread` | `0` | How far each child's size strays from `child_size`, in the same way, at least 1 lot |

Real brokers' algorithms vary the timing and size of their children, so that others cannot pick
them out of the order flow; with both spreads at 0 the children come exactly `interval` apart and
exactly `child_size` large.

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
| `kind` | `new`, `cancel`, `modify` (requests as they reach the exchange); `accepted`, `rejected`, `modified`, `filled`, `cancelled` (reports to one agent); `trade`, `top_of_book`, `phase`, `indicative` (public) |
| `agent`, `client_order_id`, `order_id` | Who and which order |
| `side`, `type`, `time_in_force` | `buy` or `sell`; `limit` or `market`; `good-till-cancel`, `immediate-or-cancel` or `post-only`. For trades, the side is the aggressor's |
| `price`, `quantity`, `leaves` | Ticks and lots; `leaves` is the open quantity after a fill |
| `liquidity`, `fee` | For fills: `maker`, `taker` or `auction`, and the fill's fee in tick-lots (negative for a rebate); `auction` marks a trade in an auction's uncross too |
| `request`, `reason` | For rejections, the request kind and why; for cancellations, why |
| `bid_price`, `bid_quantity`, `ask_price`, `ask_quantity` | For `top_of_book` |
| `parent` | For `new`: the sender's id for the larger order this one is part of, if it gave one |

A trading day adds two kinds of row. `phase` gives the new phase in `reason` (`opening auction`,
`continuous`, `halt`, `closing auction` or `closed`) and, leaving an auction, the price it
uncrossed at in `price`. `indicative`, during an auction, gives the price the book would uncross
at in `price`, the lots that would trade in `quantity` and the imbalance in `leaves`, positive when
more is bid than offered at that price; all three are empty while nothing would trade.

Updates of the depth feed are not logged: the log's orders already determine the whole book, and
`--depth` records the levels at regular times.

## Price and depth samples

`--prices` writes `time,bid,ask,last_trade` every `--sample-interval`, from time 0 to the end of
the run: the best bid and ask as the market stood at that time, after everything that happened at
it, and the price of the last trade so far. A field is empty while there is no such price. With a
`[fundamental]` or `[prediction]` section, a last column, `value`, has the true value then. A
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
| `final_value` | The true value at the end, 0 or 100 for a prediction market that resolved, or `null` without a `[fundamental]` or `[prediction]` section |
| `agent_ids` | The group's agents, as they appear in the event log's `agent` column |
| `traded` | Lots the group bought plus lots it sold |
| `cash`, `pnl` | In tick-lots; `pnl` is the change in cash plus the change in position valued at `last_price`, before fees |
| `pnl_at_value` | In a prediction market only: the same with the position valued at `final_value`, which is what a group made once the question resolved |
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
`--duration`, `--log`, `--log-only` and `--json` like `run`, and:

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
| `end_ns` | When it stopped, in simulated nanoseconds; no later than its market ends |
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
| `--feed-interval D` | Send market data every D, to every seat at once, with only the latest depth and top of book; order events still go at once. Everything as it happens unless given |
| `--spin D` | Stop sleeping D before each event is due and watch the clock instead: answers on time to the microsecond, for up to D of busy waiting per event. 0 unless given |

A recorded session replays with `replay` like one from `play`, every seat's orders included.

`connect` is the trading screen of `play` for one seat of a served market, with the same keys
except that speed and pause belong to the server. It takes `--seat`, `you` unless given, and
`--token`.

## Reports and rewinding

```bash
./build/release/apps/crowdbook report session.toml --json report.json
./build/release/apps/crowdbook play session.toml --at 2m --record retry.toml
```

`report` replays a session, and again without each seat, and prints for every seat who it traded
with, those groups' edge against it at the true value, its markout ten seconds after its fills,
its PnL at the end, and what the market would have done without it. `--json` writes the whole
report and `--interval` sets how often it samples balances and prices, a second unless given.
`play` and `serve` take `--report FILE` to write the report when the session ends.

The JSON report has `seed`, `end_ns`, `final_value` and, for each seat in `seats`:

| Field | Meaning |
|---|---|
| `seat`, `score` | The seat's name and its score, or `null` without scoring |
| `counterparties` | One entry per group the seat traded with: `group`, `type`, `fills`, `bought` and `sold` by the seat, `edge` (the group's edge against the seat at the true value, ticks a lot) and `markout_10s` (the seat's markout ten seconds after its fills, ticks a lot) |
| `fills` | Every fill: `time_ns`, `side`, `price`, `quantity`, `liquidity`, `fee`, the `counterparty` agent with its `group` and `type`, the true `value` at the fill, and the mid `mid_1s`, `mid_10s` and `mid_60s` after it |
| `pnl` | Every interval: `time_ns`, `position`, `cash`, `fees`, `pnl` net of fees at the last trade price, and `pnl_at_value` at the true value |
| `without` | The session replayed without the seat: `last_price` and `last_price_without`, `vwap` and `vwap_without`, `impact` (the mid with the seat less the mid without it, on average) and `prices` every interval, with `mid` and `mid_without` |

Values the market could not provide are `null`: the true value without a `[fundamental]`
section, a mid while one side of the book is empty, a markout past the end of the session.

`play` and `serve` take a session file instead of a scenario when given `--at`: the session is
rewound to that moment, by replaying its requests up to it, and played or served on from there
with the same seats. The new recording holds the replayed requests and the new ones. A rewound
session keeps its seed, so `--seed` and `--seat` cannot be given with `--at`. `play` rewinds
sessions with one seat; `serve` hands every seat back to whoever claims it, with its open orders
listed in the welcome under the ids they have inside the market.
