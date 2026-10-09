# crowdbook design

## Goal

crowdbook is a market realistic enough to study that you can also trade in, by hand or with your
own bot, and that tells you the truth afterwards. One engine serves four uses:

- **Research.** Simulate a continuous double-auction market as the sum of individual agents, so
  that market microstructure — spreads, depth, price impact, volatility — emerges from agent
  behaviour instead of being assumed. Measure it against real markets, and run experiments no real
  exchange allows: change the tick size, the fees or the speed of the connections, or replay the
  same market without one of its traders.
- **Practice and assessment.** Trade in the simulated market while it runs, by hand or with a bot,
  against a crowd of agents and other people, in challenges with published scoring. Afterwards,
  see what a real market never shows: who you traded with, which of them knew the true value, and
  what the market would have done without you. Every session replays exactly, so anyone holding
  its file can check a score, which is what training, interviews and competitions need.
- **Testing strategies and systems.** A backtest on recorded data cannot react to your orders;
  this market does, with price impact, queue position and informed traders who pick off stale
  quotes. Strategies and execution algorithms connect through the same gateway as everyone else,
  and later trading systems connect through industry-standard protocols, so the engine also works
  as a test exchange with realistic order flow behind it.
- **Toolkit.** Anyone can add an agent: a C++ class against a small API for the built-in crowd, or
  any program that speaks the gateway's protocol, with a Python client provided. Every run is
  reproducible from its seed and its recorded inputs.

"Realistic" has a finish line: a scorecard of statistics measured the same way on the simulated
market and on a real one (M18).

Out of scope: connecting to real exchanges or trading real money, and multiple instruments or
venues until the single-instrument market is calibrated.

This repository is the open engine, under the MIT license: the simulator, its agents, the
analysis, the terminal trading screen, the gateway with its protocol, and the Python client.
A trading screen in the browser, multiplayer markets hosted online, and tournaments with
leaderboards are planned as part of a hosted product built on the engine, and are not built here;
the roadmap marks them.

## Architecture

```
 Agent ── new / cancel / modify ──▶ [latency] ──▶ Exchange ──▶ OrderBook
   ▲                                                 │
   └── ack / reject / fill / market data ◀─ [latency] ┘
        every message is an event scheduled by the Kernel (time_ns, seq)
```

- **Kernel** — discrete-event scheduler. Events are ordered by timestamp, then by insertion
  sequence number, so simultaneous events always resolve the same way. See
  [Simulation](#simulation).
- **Exchange** — the only component that changes the book or any account. Validates and
  acknowledges orders, assigns order ids, enforces ownership and risk limits, and keeps each
  agent's cash and position. See [Exchange](#exchange).
- **OrderBook** — price-time priority matching on integer ticks with self-trade prevention.
  Supports limit, market, cancel, modify and immediate-or-cancel orders. See
  [Order book](#order-book).
- **Agents** — subclasses of `Agent` that react to callbacks (start, market data, order events,
  timer wakeups) and act through a context that can submit, cancel and modify orders, schedule
  wakeups, read the clock and draw random numbers. Agents never touch the book directly and never
  see other agents' identities.
- **Market data** — anonymous trade prints and top-of-book updates, either streamed to an agent
  or read on demand, always with the agent's own latency, so every agent acts on a slightly stale
  view, as in real markets. Depth snapshots are planned.
- **Scenarios** — a TOML file choosing the agent population and parameters, so experiments run
  without recompiling. Agents register under a name, so user-defined agents work in scenarios too.
  See [Agents and scenarios](#agents-and-scenarios) and [scenarios.md](scenarios.md).
- **Live sessions** — a person trades in a running market from a terminal screen, and the session
  replays exactly. See [Live sessions](#live-sessions).
- **Gateway** — people and programs trade in one market over the network, each from a seat of
  its own, with a protocol any language can speak, and the session still replays exactly. See
  [Gateway](#gateway) and [protocol.md](protocol.md).
- **Event log** — every request the exchange receives and every event it produces, written as CSV
  and analysed in Python (`analysis/`). Long runs can keep only chosen kinds of rows or sample
  prices at fixed times instead, and each run's results can be written as JSON. See
  [Analysis](#analysis).

## Order book

`OrderBook` holds one instrument's resting orders and does the matching. It does not assign order
ids or decide who may cancel what; the exchange does both before calling it.

- **Price levels:** one `std::map` per side, ordered so `begin()` is the best price (bids
  descending, asks ascending). Each level keeps its total open quantity and order count, so depth
  snapshots never walk individual orders.
- **Orders:** owned by a `std::unordered_map` keyed by order id, and linked into a doubly linked
  FIFO queue for their price level. Hash-map nodes never move, so the links stay valid as the map
  grows. Copying a book would leave the copy's links pointing into the original, so copies are
  disabled.
- **Fills** are appended to a vector the caller owns, so a hot loop reuses one buffer instead of
  allocating per order.

| Operation | Cost (L = price levels on one side) |
|---|---|
| Rest an order | O(log L) |
| Cancel | O(1) on average, plus O(log L) if its level empties |
| Each fill while matching | O(1) |
| Best bid or ask | O(1) |

Rules:

- An incoming order trades at the resting order's price: best price first, then oldest first.
- A limit order never trades beyond its limit. Its unfilled quantity rests at the limit
  (good-till-cancel) or is cancelled (immediate-or-cancel). Market orders take what the book has
  and cancel the rest.
- `modify` sets a new price and open quantity. Reducing quantity at the same price keeps queue
  position; any other change cancels the order and re-enters it with the same id, so it can trade
  immediately.
- Self-trade prevention: when an incoming order reaches a resting order from the same owner, the
  incoming order's remaining quantity is cancelled. Orders ahead of that resting order still trade.

On an Apple M5 (Release build, `crowdbook_bench`) the book handles about 45 million operations per
second on a mixed stream of passive orders, cancels, crossing orders and market orders, roughly
22 ns per operation. Price levels that empty keep their memory for the next new level, so a busy
book allocates almost nothing.

## Exchange

`Exchange` owns the book and every account. Agents never call the book; they send requests
(`NewOrder`, `CancelOrder`, `ModifyOrder`) and receive events.

- **Client order ids:** agents name their orders with ids they choose, unique among their own live
  orders. An agent can therefore cancel an order before its acknowledgement arrives, which matters
  once messages have latency, and can never name another agent's order. The exchange assigns the
  global order id and reports it in the acknowledgement.
- **Checks:** a request that fails any check gets exactly one `OrderRejected` with the reason, and
  nothing else changes. Quantity must be positive and within the agent's `maxOrderQuantity`; a
  limit price must be in [1, `kMaxPrice`]; a new order's client order id must not already be
  live, and a cancel or modify must name a live order.
- **Position limit:** checked as if every open order on that side filled —
  `position + open buys + new quantity ≤ maxPosition` for buys and
  `position − open sells − new quantity ≥ −maxPosition` for sells — so no sequence of fills can
  breach it. A modify is checked only when it adds quantity. Short selling is allowed within the
  limit.
- **Post-only orders** are a time in force, as on several real exchanges: the order rests like
  good-till-cancel, but the exchange rejects it, or a later modify of it, if it would trade on
  arrival. It is rejected even when the order it would meet is the agent's own, where self-trade
  prevention would otherwise step in.
- **Accounts:** cash (in tick-lots), position and fees. Cash has no limit and may go negative, so
  risk is bounded by position alone. Each trade moves `price × quantity` of cash from buyer to
  seller and `quantity` shares the other way, so total cash and total shares never change.
- **Fees:** with maker–taker pricing, every fill charges the owner of the resting order the maker
  fee and the owner of the incoming order the taker fee, per lot; a negative fee is a rebate.
  Fees are counted in thousandths of a tick-lot, so rates finer than a tick per lot stay exact,
  and kept apart from cash. The fees agents pay always add up to what the exchange collects, and
  maker plus taker fee may not be negative.
- **Events** for each request are appended in a fixed order, in the style of FIX execution reports:
  1. the sender's `OrderAccepted`, `OrderModified`, `OrderCancelled` or `OrderRejected`;
  2. for each execution, the maker's `OrderFilled`, the taker's `OrderFilled` and a public `Trade`;
  3. `OrderCancelled` for quantity that could not rest (immediate-or-cancel or market remainder,
     self-trade prevention);
  4. `TopOfBook`, if the best bid or ask changed in price or size;
  5. `BookDepth`, with a depth feed, if any of the published levels changed.

  Every request gets at least one event, and the first always answers the sender. Fills carry the
  order's remaining open quantity and its fee, so an agent can track its own orders and balances
  from its events alone.
- **Market data:** `Trade`, `TopOfBook` and `BookDepth` are public and never identify agents or
  orders. `BookDepth` is the depth feed: the best levels on each side, as many as the exchange is
  configured to publish, sent whenever one of them changes.

## Trading day

A scenario with a `[trading_day]` section trades the way a stock exchange's day goes, in phases;
without one, the market trades continuously from start to end, as before.

- **Phases:** an opening auction from the start, continuous trading, a closing auction at the
  end of the day, then closed. A halt is an auction too. The schedule's phase changes are
  actions in the kernel's queue like any other, so they happen at the same nanosecond in every
  run; a halt schedules its own end.
- **Auctions are call auctions.** Limit orders rest without trading, so the book can be crossed.
  Market, immediate-or-cancel and post-only orders are rejected, since none of them can wait for
  the call. When the auction ends, the book uncrosses at a single price: the price that trades
  the most lots; among those, the one that leaves the smallest imbalance between what is left
  to buy and to sell; among those, the one nearest the reference price, the last auction's
  price or the scenario's reference before any auction; and among those, the lowest. Orders
  trade at that price in price-time priority, both sides as auction liquidity, paying the
  exchange's auction fee. If an uncross would match an agent with itself, the newer order's
  remainder is cancelled as a self-trade and the clearing price stays as it was. Cancelling it
  takes lots out of the volume that price was chosen for, which can leave the book crossed, so
  an uncross that cancelled a self-trade is followed by another, at the price for what is left,
  until one cancels none. Without self-trades one round always leaves the book uncrossed: a
  crossing bid and ask left over would have made another price trade more.
- **The indicative price:** during an auction the exchange publishes, after every change, the
  price the book would uncross at now, the lots that would trade and the imbalance, as real
  exchanges do. The best bid and ask are still published, and may be crossed.
- **Closed:** after the closing auction nothing trades and only cancels are accepted.
- **Halts:** a continuous trade further than the halt band from the reference price halts the
  market into an auction for the halt's length. The trade that set it off stands. A halt that
  would run into the closing auction ends there instead.
- **Agents in auctions:** every agent can see the phase, streamed or in its snapshot. Noise
  traders send limit orders, which build the auction's book, and skip the market orders they
  would have sent; the market maker, momentum and informed traders wait for continuous trading,
  since their orders cannot rest; execution agents pause their parent orders and carry on
  after.
- **Intraday activity:** an optional U-shaped curve multiplies the noise traders' order rates,
  busier at the open and the close than at midday, as on real exchanges. It is an assumption,
  stated in the scenario, and the volatility and spreads that follow from it are measured.
- **VWAP:** a third execution style paces a parent order by the same curve, so it trades more
  when the market is expected to be busy, which is what tracking the day's VWAP means.

## Prediction markets

A scenario with a `[prediction]` section is a market on one yes-or-no question, such as an
election or a game, the kind Polymarket and Kalshi run. Everything else about the market is the
same: the exchange, the agents, the trading day, scoring and reports.

- **Prices** are cents, 1 to 99: a share pays 100 if the answer is yes and nothing if it is no.
  The exchange takes no limit price above 99, and an agent asks its context, `maxPrice()`, for the
  highest price it may use, so that every agent, built in or not, stays within any market's
  prices.
- **The true value is the probability of yes**, in cents. The question's answer is decided by a
  hidden quantity that wanders, with news that moves it in jumps, and the answer is yes if it
  ends above zero. Its change over the whole run has variance 1, a share of it from news at the
  scenario's rate and the rest from the wandering. The true value at any moment is the exact
  probability, given everything so far, that it will end above zero: a sum, over how many pieces
  of news are still to come, of the Poisson chance of that many times Φ(x / √(the variance
  left)). That makes the value a fair price that moves the way real ones do: slowly while much is
  unknown, and in larger and larger steps as resolution comes near, until at the end it is 0 or
  100. The run starts with the hidden quantity where the probability is the scenario's starting
  one, found by bisection, and a run that does not end on a whole step takes what is left of the
  last one, so the value is fair to the end.
- **Resolution is the end of the run.** Positions are worth 100 or 0 a share from then on, which
  is how scoring at the true value and the reports already value them; the results add each
  group's PnL at resolution beside its PnL at the last price.
- **Agents need nothing new.** Informed traders see the probability through the same beliefs as
  any true value, late and with errors of their own. They need capital, though: with too little,
  they run out of room before resolution and the price stops following the probability; and
  traders who see it late hold prices back from 0 and 100 ([results.md](results.md)).
- **One source of the value.** `makeFundamental` builds a scenario's true value, fundamental or
  probability, from its seed for the agents, the reports and the price samples alike, so they all
  see the same path without one disturbing another.
- **Normal probabilities** are computed with Marsaglia's series for Φ and the deterministic
  `math::exp`, accurate to about 1e-15, so the path is the same on every platform. The value is
  worked out once per step, about 2 µs with dozens of pieces of news still to come.

## Simulation

`Simulation` is the discrete-event kernel. It owns the exchange and the agents, and works through
one queue of agent starts, wakeups, requests reaching the exchange and events reaching agents, in
time order. Items due at the same nanosecond run in the order they were scheduled.

- **Latency:** each agent has a one-way delay to the exchange and another back, plus optional
  uniform jitter per message. Each link is first in, first out, like a TCP connection: a message
  never overtakes an earlier one on the same link, so an agent's cancel cannot arrive before the
  order it cancels. The exchange itself takes no time.
- **Agents** subclass `Agent` and override the callbacks they need: `onStart`, `onWakeup`,
  `onAccepted`, `onRejected`, `onModified`, `onFilled`, `onCancelled`, `onTrade` and
  `onTopOfBook`. Each callback gets an `AgentContext` to act through: `submitLimit`,
  `submitMarket`, `cancel`, `modify`, `wakeAt`, `wakeAfter` and `wakeWithin`, plus `now()`,
  `random()` and `ledger()`. The context assigns client order ids, so agents never manage them.
  `wakeWithin(interval)` wakes the agent at a random time within the interval; agents on a fixed
  timer start it that way, so a group of them started together does not act in lockstep.
- **Ledger:** each agent's own view of its cash, position and orders, built only from what it sent
  and what it heard back. It shows orders in flight before they are acknowledged, and an event is
  applied to it before the agent's callback for that event runs. Once nothing is in flight it
  matches the exchange exactly.
- **Randomness:** `Random` is xoshiro256** seeded through SplitMix64, with uniform, Bernoulli,
  exponential and normal distributions written here. Agent *n* draws from stream 2*n* and its
  network jitter from stream 2*n* + 1, so turning on jitter or adding another agent does not change
  any agent's own draws. Every draw is identical on every platform: floating-point draws use
  `crowdbook::math` and the correctly rounded square root.
- **Arithmetic the same everywhere:** a seeded run is byte for byte the same on every platform,
  not just on one machine. Standard libraries compute `exp`, `log` and `pow` differently in the
  last bit, and a one-bit difference sends a run down another path, so `crowdbook::math`
  implements them with addition, subtraction, multiplication and division in a fixed order, from
  tables worked out to 60 digits by `tools/math_tables.py`; they agree with the standard library
  to within one unit in the last place. Every target compiles with `-ffp-contract=off`, since
  Clang fuses multiplies and adds by default and GCC on x86-64 does not. No statement makes more
  than one random draw, because the order in which operands and arguments are evaluated is
  unspecified, and the exchange's hash maps are only looked up, never walked, outside its audit.
- **Market data:** an agent chooses how it gets public data by overriding `marketData()`.
  Streaming agents receive every trade and top-of-book change through `onTrade` and
  `onTopOfBook`. Snapshot agents receive nothing and call `context.market()` when they act, which
  returns what the exchange had published one `fromExchange` latency earlier: never anything they
  could not have seen yet. Streaming every update to every agent costs N² as the crowd grows, so
  the built-in traders that only look at the market when they act use snapshots; the market
  maker, which reacts to every trade, streams. With a depth feed, snapshots carry the published
  levels too, and streaming agents receive `onDepth`. The kernel keeps a short history of public
  states for snapshot reads and drops each state once no agent's latency can reach back to it, so
  memory does not grow with the length of a run.
- **Event log:** `CsvEventLog` writes every request the exchange receives and every event it
  produces, one row each, in processing order, optionally only rows of chosen kinds. Two runs with
  the same seed produce byte-identical logs. `PriceSampler` writes the best bid, best ask and last
  trade price at fixed times instead, which is all that return statistics over long runs need, and
  `BroadcastSink` feeds one run to several sinks.

One trap for agent authors: C++ leaves the evaluation order of function arguments unspecified, so
`context.modify(id, random.uniformInt(...), random.uniformInt(...))` may consume the random stream
in a different order on another compiler. Draw into local variables first.

## Live sessions

`crowdbook play` puts a person in the market. Nothing about the simulation changes for it: the
person trades through a `Participant` agent, added last, with the scenario's `[participant]`
account and latency, and their orders take the same path as every other agent's.

- **One way in:** `play` is a gateway with one client in the same process, exactly as `serve`
  with one seat, minus the socket. Its trading screen sends protocol messages and draws what comes
  back, the same screen `crowdbook connect` runs over the network, so the person at the keyboard
  sees only what a client would, and there is one path for orders, stop-outs and the end.
- **Pacing:** `Pacer` maps a steady wall clock to simulated time at an adjustable speed, stands
  still while paused, and never goes backwards. Each frame, about thirty a second, the gateway
  runs the market up to the paced time, the screen draws, and a key is read. Whoever runs the
  gateway can change its speed and pause it.
- **Acting from outside:** `Simulation::act` runs code with an agent's context at the current
  time, as if one of its callbacks were running. A key press becomes one call, at the moment the
  market has been run to. Everything stays on one thread, so there are no locks.
- **Recording and replay:** each request is recorded with the simulated nanosecond it was sent
  at, and a session file holds the scenario's text, the seed and those requests. A replay builds
  the market the same way, runs it up to each recorded time and sends the request there. That is
  exactly what happened live, because running to t₁ and then to t₂ processes the same items in
  the same order as running straight to t₂. The replay's event log is therefore the session's,
  byte for byte, and a new order that gets a different client order id than recorded stops the
  replay with an error.
- **The screen:** a price ladder drawn in the terminal with ANSI escape codes, with no library
  behind it. Drawing is a pure function from the screen's state to lines of text, so it is tested
  without a terminal; `RawTerminal` only switches the terminal into raw mode and back.

`ScenarioRun` is what makes this possible: `runScenario` used to build a market and run it to the
end in one call, and now builds a `ScenarioRun`, which can also run in steps and take agents of
the caller's own.

## Gateway

`crowdbook serve` runs a scenario in real time and lets people and programs trade in it over the
network, several at once. It is a live session with more than one participant, reached through a
socket instead of a keyboard, so everything in [Live sessions](#live-sessions) still holds: one
thread, the market run up to the paced time and acted on in between, and a recording that replays
byte for byte. [protocol.md](protocol.md) specifies the messages.

- **Seats:** the server is started with the names of its seats. Each seat is a `Participant`
  added after the scenario's agents, in the order named, and every seat gets the scenario's
  `[participant]` account and latency, so nobody starts ahead. A client claims a seat by name in
  its first message; with a token file, it must also give the seat's token. The clock starts once
  every seat is claimed.
- **Messages and their encoding are separate.** The protocol is a set of message types, and the
  encoding turns them into bytes. The first encoding is JSON, one message per line: any language
  can speak it, and a person can read it. A binary encoding and FIX come later (M19) as other
  encodings of the same messages. Every number on the wire is an integer, as in the engine.
- **Order ids:** a client names its orders with its own ids, as with FIX's ClOrdID, so it can
  cancel an order it has only just sent. The gateway gives each new order a fresh client order id
  inside the market and translates every event back. A wire id is free again once the order is
  done: filled, cancelled or rejected. A new order reusing a live wire id, or a cancel or modify
  naming an unknown one, is rejected by the gateway without reaching the market, as a real
  exchange's gateway rejects malformed orders before its matching engine sees them.
- **When a message counts:** each message is stamped with the paced simulated time at which the
  server reads it, and sent from the seat's agent at that time, on the seat's simulated latency.
  The recording keeps that time and the seat, so a replay sends it at the same moment from the
  same agent. Real network delay therefore adds to the simulated one, scaled by the speed: at 50
  times real time, a millisecond on the wire is 50 simulated milliseconds. That is inherent to
  trading in real time; the learning environment (M20) will step the market in lockstep instead.
- **Untrusted input:** a line longer than 4 KiB, a malformed message, an unknown field or a value
  out of range gets an error message and is dropped. Each connection may send a limited number of
  messages per second of wall-clock time; messages over the limit are dropped before they reach
  the market, so the limit never affects a replay. An order request over the limit is rejected
  by its id, so the client's books stay right, and anything else gets an error. A client that does
  not read its messages fast enough is disconnected once 8 MiB are waiting for it, and a
  connection that does not claim a seat within five seconds is closed.
- **Closing politely:** a connection the server is done with is shut for writing, then read and
  discarded until the client hangs up or two seconds pass. Closing it with the client's messages
  still unread would reset it, and a reset can discard the last messages sent to the client, the
  session's end among them.
- **Disconnecting** cancels every open order of the seat, as cancel-on-disconnect does on real
  exchanges; the cancels are recorded like any other request. The seat can be claimed again, and
  the welcome message then carries its cash, position, fees and open orders.
- **Sockets:** non-blocking sockets on the market's thread, waited on with epoll on Linux and
  kqueue on macOS and the BSDs, which time out to the nanosecond where `poll` rounds to a
  millisecond. The server sleeps until the market's next event is due or a client sends
  something, so an order's answer leaves when the market makes it rather than on the next tick of
  a timer; on Linux it asks the kernel for its wakeups on time while it serves. `--spin` has it
  stop sleeping a little before each deadline and watch the clock instead, for answers on time to
  the microsecond at the price of that much busy waiting per event. The gateway reports which
  connections have something new and when it is next due, so the server never scans every
  connection. The server listens on 127.0.0.1 unless told otherwise, so a market is not
  reachable from other machines by accident.
- **Market data for many seats:** a public update reaches every seat with the same latency at the
  same moment, and is encoded once for all of them. With `--feed-interval`, market data goes out
  on one shared tick, to every seat at once, so that none sees the market before another, and a
  depth or top of book update still waiting when a newer one comes is dropped: snapshots
  supersede each other. A seat's own order events, errors and the start and end go at once. At
  200 seats and a 10 ms tick, this cuts what the server sends from 500 MB to 17 MB a second.
- **Layers:** the gateway itself only turns bytes from connections into requests and events into
  bytes, and never touches a socket, so tests drive it with byte strings and no network. The
  server around it moves bytes between sockets and the gateway, and the clock between the wall
  and the market.

The Python client in `clients/python` speaks the protocol with the standard library alone, so
it installs anywhere without pulling in anything. A bot subclasses `Bot` and overrides callbacks,
as an agent inside the market subclasses `Agent`: `on_start`, `on_trade`, `on_filled` and the
rest, with `buy`, `sell`, `cancel` and `wake_after` to act. It keeps the seat's ledger by the same
rules as the C++ `Ledger`, and the market's best prices and depth from the public messages. One
thread reads the socket with `select`, so a bot's callbacks never run concurrently. It decodes
the server's messages leniently, ignoring fields it does not know, so that a newer server can add
fields without breaking older clients; the server, which must not trust its input, is strict.

`crowdbook connect` is the terminal trading screen as a client of a served market. It keeps the
seat's ledger from its own requests and events, and the market from the public messages, and is
the same screen as `crowdbook play`, without the speed and pause keys.

## Scoring and challenges

A scenario with a `[scoring]` section scores each seat of a session; with a `[challenge]` section
it is also a challenge, with a name and a briefing that clients show before the clock starts.
The engine computes the score from the exchange's events, the same way live and in a replay, so a
replay recomputes a session's score exactly and anyone holding the session file can check it.

Scores are in points, thousandths of a tick-lot, the unit fees already use, so that every number
on the wire stays a whole number. Rates in the scenario are written in ticks, with at most three
decimals, and kept as whole points.

- **PnL** is the change in cash plus the change in position valued at the mark, net of fees. The
  mark is the last trade price, or with `mark = "value"` the true value at the end, which scores
  what a position was really worth rather than where the last trade happened to print.
- **Inventory** costs `inventory_penalty` per lot per second held, integrated over the session
  from the seat's fills: holding risk costs something even when it pays off.
- **Closing** costs `close_penalty` per lot still held at the end.
- **A target** is a large order to work: a side and a quantity. The score is measured against a
  paper portfolio that traded the whole target at a benchmark price, either the market's VWAP
  over the session or the reference price: the seat's PnL minus the paper portfolio's PnL, at
  the same mark. Trading the target at the benchmark scores zero; doing better scores above
  zero. Lots of the target not done cost `unfinished_penalty` each on top, since the paper
  portfolio's PnL alone would reward not trading when the price ran away.
- **A loss limit** stops a seat whose loss, valued at the latest trade price, reaches
  `max_loss`: its open orders are cancelled and it can send no more orders, as a risk manager
  would cut off a trader. The scorer checks at every trade, so the moment is the same in a
  replay; the cancels are recorded like any other request. The seat is still scored at the end,
  with whatever position it was left holding.

    score = pnl − inventory − closing − paper pnl (target only) − unfinished (target only)

Each part is computed in floating point from whole numbers and rounded to points once, with the
same arithmetic on every platform.

Challenges are scenario files in `examples/challenges`: making markets within a risk limit,
working a large order, trading the news, and making markets against informed traders. The example
bots play every one of them in CI, and each session's score from the server must equal the score
of its replay.

## Session reports

`crowdbook report` reads a session file and tells each seat what a real market never would. It
works from the recording alone, by replaying it, so anyone holding the file gets the same report;
`play` and `serve` write one at the end when asked.

- **Who you traded with.** The exchange reports each execution as the maker's fill, then the
  taker's, then the trade, so a replay pairs every fill of the seat with the agent on the other
  side, and the scenario's groups name it: noise, the market maker, an informed trader.
- **What they knew.** For each fill, the true value at that moment, and the mid one, ten and sixty
  seconds later. Summed by counterparty, this is each group's edge against the seat: how far the
  price was from the true value in their favour when they traded with you. Informed traders see
  the value, so their edge is the price of trading with them.
- **PnL over time.** The seat's position, cash and PnL, at the last trade price and at the true
  value, every second.
- **The market without you.** The session replayed without the seat's orders: the same seed and
  everyone else's orders as they would have been, a market in which the seat never traded. The
  report compares its prices with the real session's, which measures the seat's impact. The
  replay keeps the seat's participant, which sends nothing, so every other agent keeps its id and
  random streams; its event log is byte for byte the log of the scenario run with no participant
  at all, which a test checks.
- **The true value, after the fact.** A report builds its own copy of the true value from the
  scenario's seed. The value draws only from its own random stream, so the copy's path is the one
  the market had, and reading it at any time changes nothing in the replay.

Rewinding is a recording cut short: `play --from session.toml --at 2m` and the same for `serve`
replay a session's requests up to that moment, then hand its seats to people and programs again.
The new recording holds the replayed requests and the new ones, so it replays like any other.

## Agents and scenarios

Five agent types are built in. Each is a plain class configured by a struct, so it can be used
from C++ directly; [scenarios.md](scenarios.md) lists their parameters.

- **Zero-intelligence trader** (Farmer, Patelli and Zovko, 2005): limit and market orders at
  Poisson times, on random sides, with limit prices drawn inside the opposite best quote. Each
  resting order is cancelled after its own exponentially distributed lifetime. Optionally it
  reacts to the market: its pace follows recent activity against its usual level, and its limit
  orders stand further back from the best prices when prices have been jumping more than usual.
  It measures both from its own snapshots, which carry the trades and volume published so far.
- **Market maker** (Avellaneda and Stoikov, 2008): one bid and one ask around a reservation price
  that leans against inventory, requoted on a timer and after every fill. The examples run two,
  a fast one at the touch and a slower one quoting more lots further out, so the book has
  competition and depth. Its fair price is a
  running average of trade prices rather than the mid, because the mid is often its own quotes.
- **Momentum trader:** market orders in the direction of a fast moving average's lead over a slow
  one, within a position limit that counts orders in flight.
- **Informed trader:** follows the true value as its belief sees it and trades with
  immediate-or-cancel orders priced to keep its edge, so it never sweeps the book past its
  estimate. It does nothing while the price is within its range of fair values.
- **Adaptive trader** (Brock and Hommes, 1998): switches between a value strategy and a trend
  strategy by the track record of each, choosing by a logit of the difference, and holds the
  position its chosen strategy calls for. Traders like it herd, since they all score the same
  price moves.
- **Execution agent:** a broker's algorithm working parent orders of a Pareto-distributed size,
  one at a time, with child market orders: an even pace in time (TWAP), or a share of the market's
  volume (POV). Each child carries the parent's id, which the event log keeps.

The fundamental value is an Ornstein–Uhlenbeck process (a random walk when its mean reversion is
zero, the default) stepped with its exact discrete-time formulas from its own random stream, so
its path does not depend on who reads it or when. News adds jumps at random times; without news it
makes no draws for them, so turning news off leaves every earlier path as it was.

Nobody knows the value exactly. Informed and adaptive traders see it through a `Belief`: as it
was a moment ago, by a delay of the trader's own; with a lasting error that wanders back and forth
(an Ornstein–Uhlenbeck process sampled at each look); tilted by the group's bias; and with fresh
noise on each look. The value remembers its recent steps for traders who see it late, and a read
further back than any trader's delay is an error. A belief draws its fresh noise first and draws
nothing else unless its other settings are on, so a scenario that sets none of them runs exactly
as before and old sessions replay. The examples use crowds: a few experts, quick and close to
right, many followers, late and with views of their own, and in prediction markets partisans.
Capacity comes from the size of the crowd, on ordinary position limits, as it does in real
markets, and as the price strays further from the value more of the crowd finds it outside their
range and trades it back. Holding a price to its value for a whole day takes a large crowd: the
research markets have four hundred followers on 1,000 lots each and ten experts on 5,000.

A scenario names agent groups with shared settings. `AgentRegistry` maps type names to factories
that read a `Parameters` object; after a factory runs, any parameter it did not read is an error,
so a misspelled key cannot silently fall back to a default. `runScenario` builds the market from
a `Scenario` struct and reports trades, volume, the last price, the fundamental's final value and
each group's position, cash and PnL. Scenario files are only one way to fill in that struct: the
TOML reader lives in its own library, so the core library stays free of dependencies.

Running the examples and analysing the results taught eight things worth keeping.

**Market-maker self-impact.** In Avellaneda–Stoikov the mid price is
exogenous, but here the zero-intelligence traders anchor on the best quotes, which are often the
market maker's own. The market maker's inventory skew, γσ²τ per lot, therefore moves the market,
and with the paper's parameters, rescaled to ticks, the skew was large enough to drag prices
against its own inventory: it lost money even against pure noise, and more the wider it quoted.
With a skew of about 0.05 ticks per lot it earns the spread. The defaults are set for that regime,
and [results.md](results.md) measures how this self-impact depends on the skew and on how much
liquidity others provide.

**Cancel per order, not per trader.** The zero-intelligence traders first cancelled one of their
own orders at a fixed rate per trader. Orders then arrived faster than they were cancelled, the
book grew without limit (from 4,500 resting orders to 12,500 in ten simulated minutes, with a
thousand traders), and the orders piling up around the early prices pinned the price for hours.
Giving each order its own exponential lifetime makes total cancellations grow with the book, which
settles at a steady size, about a thousand orders for that market, and the price then wanders
freely.

**Desynchronize timers.** Momentum and informed traders first started their timers at exactly
one interval. Every agent of a group then acted within microseconds of the others for the whole
run, so twenty informed traders hit the book as one burst five times a second. Measuring price
impact by trader type exposed it: the price kept moving within a millisecond of an informed order,
as the rest of the burst arrived. Timers now start at a random point within the first interval.

**Size limits to the flow an agent must absorb.** Informed traders who hold the price to the true
value must take the other side of the noise traders' net order flow, which wanders like a random
walk: tens of thousands of lots over a simulated day. With limits of a hundred lots each they
filled up within half an hour, the price then drifted hundreds of ticks from the value, and the
fat tails that market showed were an artifact of the drift. Long runs need a check that every
agent can still act, so the analysis reports each group's volume and PnL.

**Memory comes from feedback on volatility, not on activity.** Making the noise traders' pace
follow recent activity changed almost nothing: a thousand traders already trade hundreds of times
a second, so activity barely fluctuates and there is little for the feedback to amplify. Trades
per minute varied by four percent. Having them place their limit orders further back when prices
have been jumping more than usual made volatility cluster for hours, because it closes a loop
around volatility itself: jumps thin the book, and a thin book makes the next jump bigger.

**Impact needs someone who does not know and a book that remembers.** Brokers working parents of
random side moved the price by less than half a tick in a market whose informed traders see the
true value: they absorbed the flow, as they should absorb anything that carries no information.
Without them, impact grew almost linearly with a parent's size, because noise traders' orders
lasting five seconds give the book no memory of where the price has been. Letting those orders
rest for 50 seconds bent impact toward the square root of real markets.

**Traders who know too much make the market too good.** Informed traders who saw the true value
with fresh noise on each look knew it exactly between them: the prediction market's prices were
calibrated by construction, and the price never strayed. Giving each trader a delay of its own and
a lasting error broke both, in the way real markets break: prices underreact to news, cheap
prediction shares are too dear and dear ones too cheap, and trend followers profit from the
underreaction.

**A crowd that trades small differences is a book of its own.** Four hundred followers, each
trading the price back when it was 2.5 ticks from what it thought the value, had views spread
around the value, so together they stood ready at every price near it: depth that does not thin
when the noise traders step back. Volatility stopped clustering. Followers who act only at 4
ticks, as slow money acts only on clear mispricings, leave the book to the noise traders, and the
clustering came back.

## Analysis

`analysis/` is a Python project, managed with [uv](https://docs.astral.sh/uv/), that drives the
Release build of `crowdbook` through its command line and reads its CSV and JSON output, the same
interface any user has. Nothing in the C++ build depends on it.

- `log` reads event logs and price samples into [polars](https://pola.rs) data frames, pairs
  each execution's maker with its taker, and puts parent orders back together from their
  children.
- `facts` holds the statistics: returns over a horizon, excess kurtosis, autocorrelation, tail
  distributions, volatility at each horizon (the signature plot), the time-weighted spread
  distribution, the mid's move after aggressive orders by group and horizon, markouts of each
  execution for the agent whose resting order it filled, and the impact of parent orders.
- `runner` writes a scenario, runs it, and runs many in parallel.
- `style` gives every chart the same thin marks and recessive axes, with series colors checked as a
  set for color-vision deficiencies.
- `crowdbook-facts` measures stylized facts of the large example markets,
  `crowdbook-experiments` runs the market-maker experiments, and `crowdbook-large-orders` measures
  the memory of order signs and the impact of parent orders. Each prints its tables as Markdown
  and saves its charts to `docs/images`; [results.md](results.md) reports what they found.

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-facts
uv run --project analysis crowdbook-experiments
uv run --project analysis crowdbook-large-orders
```

## Testing

- **Unit tests** spell out each matching rule with a small hand-checked scenario.
- **Differential tests** send the same random submits, cancels and modifies to `OrderBook` and to
  `ReferenceBook`, a deliberately naive book that scans a flat list for every operation, so its
  rules can be verified by reading it. Every result, every fill and the full book state must match
  after every step, for 40 seeds of 2,000 steps. Half the seeds use a narrow price band and three
  owners, so crossing orders, partial fills and self-trades are frequent.
- **Invariant audit:** `OrderBook::audit()` checks the internal structure (book not crossed, no
  empty levels, queue links, level totals, index consistency) after every random step and at the
  end of every unit test.
- **Exchange unit tests** pin down the exact event sequence for each rule.
- **Randomized exchange tests:** six agents with different limits send random requests, including
  invalid ones, post-only orders and some from an agent with no account, for 30 seeds of 2,000
  steps, on an exchange with maker–taker fees and a three-level depth feed. After every step:
  - `Exchange::audit()` cross-checks accounts, live orders and the book, and checks that cash and
    shares are conserved, the fees agents paid add up to what the exchange collected, and no
    position limit can be breached;
  - the top-of-book and depth feeds must agree with the book;
  - a post-only order must never have taken liquidity;
  - each agent's ledger, rebuilt only from its own events, must match its account, fees and open
    orders exactly.
- **Random tests** compare the generator with reference outputs from an independent Python
  implementation, itself checked against the algorithms' published values, and check each
  distribution's moments. CI runs them on macOS and Linux.
- **Simulation tests** check latency arithmetic, first-in-first-out links under jitter, wakeup
  order, start times, same-time ordering, that the ledger is updated before callbacks run, and
  that jitter leaves agents' own random draws alone.
- **Determinism test:** eight random traders with different latencies, all with jitter, trade for
  half a simulated second: about 4,000 requests, 1,200 trades and 11,000 log rows. Two runs with
  the same seed must produce byte-identical CSV logs and a different seed a different log. Once
  every message has landed, each agent's ledger must match the exchange exactly.
- **Golden test:** a market with every agent type and every source of randomness runs for four
  simulated minutes, and the hashes of its event log, price and depth samples and results, and of
  the replayed logs of the demo session, of a session served to two clients and of a scored
  challenge, and the log of a trading day with halts, must equal committed values, and so must
  the challenge's score. CI runs it on macOS
  arm64 and on Linux x86-64, so the two platforms have to agree byte for byte. `crowdbook::math` is checked
  against the standard library over its whole range, and a test fails if the compiler fuses a
  multiply and an add.
- **Agent tests** run each built-in agent against `FakeContext`, a stand-in for the simulation
  that records what the agent sends and lets the test play the exchange. They check decisions
  exactly: Avellaneda–Stoikov quotes against hand-computed values, prices relative to the book,
  position limits including orders in flight, the mix and timing of random actions, and that
  agents started together spread their first decisions over the first interval.
- **Scenario tests** run small markets and check that the groups' cash, positions and PnL sum to
  zero and that the same scenario gives the same result. Scenario file tests check that every
  setting is read and that syntax errors, unknown keys and bad values are reported with their line.
- **Snapshot tests** check that an on-demand read sees an order exactly one latency after the
  exchange published it, not a nanosecond sooner, and that over a busy run every snapshot equals
  what a streaming agent with the same delay had received by then. Planted bugs in the history
  (dropping a state that was still needed, an off-by-one in visibility, streaming to snapshot
  agents) were each caught.
- **Example tests:** CTest runs every scenario in `examples/scenarios` and the custom agent
  example, so none of them can quietly break.
- **Mutation check:** before each randomized suite was committed, deliberately planted bugs were
  each caught. Six in the order book: an off-by-one limit check, LIFO instead of FIFO, no
  self-trade prevention, market orders resting, size increases keeping priority, stale level
  totals. Eight in the exchange: open quantity not released on fills, a flipped seller cash sign, a
  position check ignoring open orders, stale live-order records, unchecked modify increases,
  misreported fill and cancel quantities, missed top-of-book updates. Seven in the kernel: links
  that reorder messages, no same-time tie-break, the ledger updated after the callback, market
  data sent to one agent only, jitter drawn from the agent's own stream, wakeups scheduled in the
  past, start times ignored. Later features got the same treatment: four planted bugs in post-only
  orders, six in fees, seven in the depth feed, three in the market maker's post-only quoting and
  eight in live sessions (the participant's settings ignored, actions replayed early, a replay
  stopping at its last action, unescaped session text, out-of-order actions accepted, recorded
  ids unchecked, a pacer that ignores pause), five in the adaptive trader, three in the noise
  traders' responses and sixteen in the gateway (seats not recorded, the cancels of a
  disconnect not recorded or not sent, wire ids never freed or freed before late answers,
  events with the market's ids, a rate limit that never spends, a hello timeout a nanosecond
  late, no limit on waiting output, a clock that starts with the first seat, long lines not
  skipped, live ids reused, a taken seat claimed twice, tokens unchecked, no clock messages, an
  end past the duration) were each caught. The gateway's test for answers that arrive after an
  order is done first ran in a market without latency, where the gateway itself answered, and
  missed its bug; with latency it catches it. Sixteen more in scoring and stop-outs (fees left
  out, inventory counted after the fill, the close penalty on the change of position, the last
  price as the benchmark, sell targets counted backwards, a loss limit one tick late, a stop
  time that moves, the true value ignored, the paper portfolio's sign, stop-outs never checked,
  a stopped seat still trading, a stop that cancels nothing, an end without the score, a
  welcome without the scoring, a scorer that hears nothing, seats it does not know) were each
  caught, three of them only once the tests pinned the exact boundary: a loss equal to the
  limit, a deeper loss after the stop, and a position held from the start. Fifteen in reports,
  replays without a seat and rewinding (each side its own counterparty, the edge from the seat's
  side, the markout a second on instead of ten, the mid of the change before, markouts past the
  end, values from another seed, PnL at value taken at the last price, cash moving the wrong
  way, a market without the seat that keeps it, seats to leave out kept, a rewind that keeps
  every action or stops short of its moment, rewound orders under no id, rewound actions not
  recorded, a rewound gateway left at the last action) were each caught, four only after the
  tests were made to tell them apart: a market whose mid moves between one and ten seconds, a
  true value that stands still, and a rewind to a moment after the last action. Twenty-nine in
  the trading day were each caught: nine in the uncross (ties that skip the imbalance, prefer the
  farthest price or the highest, the newer order as maker, supply counted a level late, asks above
  the price trading, calls that still match, a crossed call failing its audit, time priority
  ignored), ten in the exchange (auctions taking market orders, the close taking orders, one
  uncross round only, the auction fee charged once, a reference that never moves, a halt band a
  tick wider, auctions that keep matching, no indicative price on entering an auction,
  self-trades keeping their open quantity, leftovers counted from one side of a fill), three in
  the simulation (a halt's end ending any phase, halts that never end, snapshots without the
  phase), five in the agents (auction orders dropped, orders after the close, a pace without the
  curve, children in auctions, auction rejections abandoning the parent) and two in the
  scenario's schedule. Three of them, the reference price, the edge of the halt band and the
  indicative price on entering an auction, were at first caught only by the golden hash, which
  catches any change at all; direct tests now catch each.
- **Session tests** play a scripted session the way a live one runs, in uneven steps with requests
  at chosen nanoseconds, then write it, read it back and replay it: the event log must come out
  byte for byte the same. The real `play` command was also driven through a pseudo-terminal with
  scripted key presses, and its live log and the replay of its recording matched byte for byte.
  The ladder's drawing is tested on fixed screens, with and without colors.
- **Protocol tests** round-trip every message, check that malformed ones are rejected (unknown,
  repeated or mistyped fields, numbers outside int64 or with fractions, text outside ASCII,
  nesting too deep), and mangle valid lines 40,000 times at random: each must decode or be
  rejected, never crash, and whatever decodes must encode to a line that decodes the same.
- **Gateway tests** drive the gateway with byte strings and a fake wall clock, no sockets: the
  clock starting with the last seat, the client's ids on every event, a cancel before the
  acknowledgement, an answer that arrives after its order is done, the gateway's own rejections,
  the rate limit, the limit on waiting output, long and split lines, hello timeouts, tokens, a
  seat claimed again, clock messages and the end. A session with two seats, a disconnect and a
  seat claimed again replays to the same event log byte for byte.
- **Gateway fuzzing:** for 12 seeds, three seat holders trade while dozens of hostile
  connections come and go: wrong seats and tokens, orders with ids, prices and quantities at
  and past every edge, mangled lines, random bytes, lines far longer than any message, floods
  over the rate limit, readers that take a few bytes at a time or nothing, and the operator
  changing the clock's speed and pausing it. The gateway must never throw, every byte it sends
  must form whole messages, no connection may hold more output than its limit, the book must
  pass its audit, and the session must replay to the live event log byte for byte. It found that
  a session stopped before every seat was claimed never ran what was due at time 0, which its
  replay does.
- **Scoring tests** score hand-made fills and trades against values worked out by hand: PnL
  net of fees at both marks, inventory over every change of position, the close penalty, a
  target against VWAP and against the reference price, a target done at the benchmark, and the
  loss limit stopping at the trade that reaches it. A gateway test stops a seat whose fees take
  it past its loss limit: its resting order is cancelled, its next order is refused, and the
  replay's event log and score both match the live session's.
- **Report tests** check a report against hand-built sessions: each fill paired with the right
  counterparty, markouts and edges to the tick in a market whose mid and value are known, every
  fill and lot accounted for by the counterparties, the last PnL and the score equal to the
  session's results, true values equal to an independent copy of the value's path, no impact
  from a seat that never trades and a positive one from a seat that buys a hundred lots, and the
  same JSON every time. Replaying a session without its seat gives the log of the scenario run
  with no participant, byte for byte. A rewound session matches the original up to the moment,
  its seats get their open orders back under their ids, and what is played on replays exactly.
- **Auction tests** check the uncross against a deliberately naive reference that tries every
  price and sorts every order, through random flow for 40 seeds that goes in and out of call
  auctions: the indicative price after every step, and each uncross's fills, self-trades and
  book; after each uncross the book must not be crossed. Hand-worked books pin each tie rule. A
  randomized exchange test drives 30 seeds through random phase changes, halts set off by a
  narrow band, and the close, auditing conservation, ledgers, feeds and the indicative price after
  every step. Scenario tests check the day's schedule, that auctions trade only at their uncross,
  that a halt lasts its length unless the close comes first, and that a served day with its own
  duration replays byte for byte; a client fed only the protocol sees the phase and the
  indicative price as the server does.
- **Client model tests** feed a client only the protocol messages for its seat while it trades,
  modifies and cancels for 200 steps: its ledger and its view of the market must equal the
  server's exactly, before and after the seat is claimed again.
- **Server tests** run the server on a real socket on 127.0.0.1 with clients in another thread:
  trading, a hang-up that cancels orders, and two clients whose session replays byte for byte.
  The real `connect` command was also driven through a pseudo-terminal against `serve`, and the
  served session's log and its replay matched byte for byte.
- **Python client tests** check the client's encoding against the specification's examples and
  its ledger's rules on hand-made events. Run from CTest, a bot trades against the real server:
  an order over the size limit and one past the position limit are rejected for those reasons,
  every fill's fee is the exchange's rate for its side times its size, every answer from the
  exchange arrives exactly one round trip after the request left by the recording's clock, the
  bot's ledger ends equal to the server's results, and the recording replays. The two example
  bots then trade side by side in one served market, and the example bots play every challenge:
  each session's score from the server must equal the score of its replay.
- **Package tests** install the build into its own tree, then configure, build and run
  `examples/consumer`, a project of its own, against the installed package. CI also builds the
  container image and runs a market and a replay in it.
- **Format tests** refuse a scenario for a newer crowdbook, read session files that say which
  crowdbook recorded them and older ones that do not, and name the recording crowdbook when a
  replay diverges. Three recorded sessions, from three formats and three versions, replay with
  pinned logs and, for the challenge, a pinned score.
- **Analysis tests** check each statistic in `analysis/` on inputs with known answers: a random
  walk's flat volatility signature, a normal sample's zero excess kurtosis, hand-computed spreads
  and price moves.
- **Fault injection:** `crowdbook_fault_tests` replaces `operator new` with one that fails on
  demand. It fails allocation after allocation in an id map as it grows, in 400 young order books
  through random orders, modifies, cancels and auctions, and in a whole market during a busy
  minute. The id map must keep every entry; the book must pass its audit after every failure and
  still cancel every order at the end, a cancel must never allocate, and when the fills have room
  a failed request must leave the book exactly as it was; the market must stop, refuse to go on,
  and be destroyed cleanly. It runs under the sanitizers too. It found an id map that lost its
  table when growing failed, a resting order indexed before its level existed, and fills
  recorded after the order they emptied had changed.
- **Sanitizers:** the `asan` preset runs everything under AddressSanitizer and
  UndefinedBehaviorSanitizer, locally and in CI.

## Performance

Speed is part of the product: markets per core decide what a hosted competition costs, how many
seeds a study can afford and how fast a learning agent trains. Every feature is measured, and two
kinds of guard keep it fast.

- **The hot path:** everything done per request or per event.
  - Nothing goes to the heap once a run is warm, apart from depth updates. Resting orders live in
    pooled book nodes, per-order lookups go through flat id maps (open addressing in one array),
    an agent's ledger is a sorted vector, and buffers such as the exchange's events and fills, the
    log's row and a connection's output keep their memory. Queues that turn over with every event
    are rings that reuse their slots, never `std::deque`, whose blocks are 512 bytes in libstdc++:
    a deque of the kernel's public states allocated on every other market change on Linux.
  - Market data is shared, not copied. A depth update's levels (`Levels`) are allocated once, and
    every event, delivery and snapshot that holds them shares them. A side of the book the last
    request did not touch is not looked at again.
  - Text is written with `std::to_chars` into reused buffers, not with iostreams or
    `std::format`. The event log writes each row in one call, and the protocol encodes straight
    into a connection's buffer.
  - Independent work runs side by side when that cannot change the result: a report's replays use
    every core and give the same report on any number of them.
  - Determinism comes first. No optimization may change a seeded run, and the golden tests pin
    event logs byte for byte.
- **Allocation tests** (`tests/allocation_test.cpp`) count heap allocations per request over
  five minutes of steady trading: in a market with a depth feed, in the thousand-agent market,
  with the event log, and while serving a seat. Each fails above a ceiling a little over today's
  count, which is about one allocation per request with a depth feed (the shared levels) and 0.04
  without. Unlike timings, the counts do not depend on the machine, so CI enforces them.
- **Benchmarks** (`bench/`) cover every feature: the book, whole markets by size, the challenges
  alone and served, an order through the gateway, the memory each served market takes, the event
  log, the protocol's encoding and decoding, a trading day, reports on one thread and on four,
  and a large market. `crowdbook_load` puts the server under load: clients over TCP, each
  placing and cancelling orders while it reads the full market data, timing every answer at the
  50th to the 99.9th percentile against the latency the market itself imposes. CI runs both on
  its Linux machine and shows the figures on each run. `tools/bench_check.py` runs them five times,
  compares the medians with `bench/baseline.json`, recorded on the development machine, and fails
  if anything is more than 10% slower. `--update` records a new baseline when a change is meant to
  move it.

## Decisions

Choices for later milestones may change once they are implemented; changes are recorded here.

| Topic | Choice | Reason |
|---|---|---|
| Agent access | Messages through the exchange only; no direct access to the book | Makes latency, ownership checks and risk limits possible; no agent can corrupt the book or touch another agent's orders |
| Order ids | Agents name orders with their own client order ids; the exchange assigns order ids | Agents can cancel or requote before an acknowledgement arrives, and can never name another agent's order |
| Accounting | Kept by the exchange; total cash and shares are conserved, checked by tests | PnL only means something if no fill is ever lost or double-counted |
| Risk limits | Position limit with open orders counted as filled; no cash limit | The limit holds whatever fills; cash only tracks PnL, so risk is bounded by inventory, which is what market makers manage |
| Short selling | Allowed within the position limit | Market makers routinely go short; long-only would need share endowments and make buying and selling asymmetric |
| Bounds | Prices up to 10⁹ ticks, order sizes up to 10⁹ lots | Every notional and risk sum stays far inside `int64` |
| Events | Ordered, FIX-style execution reports plus anonymous trades and top of book | Agents can rebuild their own state from their events, and logs replay in a well-defined order |
| Time | `int64` nanoseconds, discrete events | Latency and queue position matter in microstructure; fixed rounds hide both |
| Ties | Same timestamp → insertion order | Deterministic; no hidden dependence on container iteration order |
| Event queue | A heap of small (time, sequence, slot) entries, with the actions in a deque beside it, built and processed where they lie | Once an event could hold the depth feed's vectors, moving whole actions around the heap took most of a run's time |
| Prices and sizes | `int64` ticks and lots; cash in tick-lots | Exact arithmetic; no floating point in matching or accounting |
| Priority | Price, then time | Standard for continuous limit order books |
| Modify | Size decrease keeps queue position; price change or size increase loses it | Matches common exchange rules, so queue-position effects are realistic |
| Self-trade prevention | Cancel the incoming order's remaining size | An agent never trades with itself and the book never ends up crossed |
| Market orders | Fill against the book, cancel any remainder | Market orders never rest |
| Links | First in, first out per agent and direction, even with jitter | Like a TCP connection: a cancel can never overtake the order it cancels |
| Market data delivery | Streamed or read on demand, chosen by each agent | Streaming to everyone costs N²; most agents only need the market at the moment they act |
| Randomness | xoshiro256** streams from the run's seed: 2*n* for agent *n*'s draws, 2*n* + 1 for its jitter | Same seed → same run; adding an agent or turning on jitter does not change any agent's draws |
| Distributions | Implemented in crowdbook, not `std::*_distribution` | Standard distribution output is implementation-defined, so libc++ and libstdc++ disagree for the same seed |
| Elementary functions | crowdbook's own `exp`, `exp2`, `log` and `pow`, from basic arithmetic, and no fused multiply-adds | The platforms' libraries differ in the last bit, which is enough to make a seeded run differ between macOS and Linux |
| Agent state | A ledger built from the agent's own requests and events, not a view of the exchange | Agents act on what they could know, including orders in flight and cancels that race fills |
| Output | Full event log rather than summaries | Microstructure analysis needs every order, cancel and trade, not end-of-run PnL |
| Log format | One CSV, a row per request or event, unused columns empty | Loads into Python in one call, and runs can be compared byte for byte |
| Scenario format | TOML, read by toml++ in a separate library | Comments can document an experiment's choices; the core library keeps no dependencies |
| Unknown parameters | An error, not ignored | A typo would otherwise run the experiment with a default nobody chose |
| Market maker's fair price | A running average of trade prices | The mid is often its own quotes; skewing around them made prices run away |
| Direction | One engine for research and play: the same agents and rules in a batch experiment and in a live session | Experiments then describe the market people trade in, and every played session is reproducible data |
| Realism | Defined by a scorecard measured the same way on simulated and real data | Without a target, tuning never ends |
| Gateway encoding | Messages defined apart from their encoding; JSON lines first, binary and FIX later | Any language can speak JSON lines and people can read them; industry encodings are then additions, not rewrites |
| Wire order ids | Chosen by the client and translated by the gateway | A client can cancel an order before its acknowledgement, as agents inside the market can |
| Seats | Named when the server starts, recorded in the session; every seat has the scenario's participant account and latency | Fair by construction, and a replay builds the same agents in the same order |
| Arrival time | The paced simulated time when the server reads a message | The only time the server can know; recording it is what makes a session with many participants replay exactly |
| Disconnects | Cancel the seat's open orders | Protects a participant whose connection drops, as exchanges' cancel-on-disconnect does |
| Server | POSIX sockets and `poll` on the market's thread, listening on 127.0.0.1 by default | No locks and no dependency; nothing is exposed to other machines unless asked |
| Python client | Standard library only, one thread with `select`, bots as subclasses with callbacks | Nothing to conflict with a user's environment; the same shape as agents inside the market; no locking in user code |
| Score units | Points, thousandths of a tick-lot, whole numbers | The fee unit already; integers on the wire, and exact sums |
| Scores | Computed by the engine from the exchange's events, live and in replays alike | A score anyone can recompute from the session file is one a competition can trust |
| Loss limit | Stops new orders and cancels open ones; the seat is still scored at the end | What a risk manager does, deterministic from the events, and no way to escape a loss by being stopped |
| Large orders | Scored against a paper portfolio filled at the benchmark, plus a penalty per unfinished lot | Implementation shortfall, the standard measure, in one formula; the penalty makes finishing the order matter |
| Counterfactual | The session replayed without the seat's requests, its participant kept but silent | Every other agent keeps its id and its random streams, so the difference is the seat's doing alone |
| Reports | Built from the session file by replaying it | A report is as checkable as a score; the live market carries no extra bookkeeping |
| Rewind | A session file cut at a moment, then played on | Nothing new to save or restore: the recording already determines every state |
| Library | Four CMake targets, installed with a package config; formats versioned apart from the library | A host links only what it uses; a file's version changes only when its format does |
| Compatibility | Old session files replay in CI with their logs and scores pinned | A change to how markets behave has to be made on purpose |
| Container | A two-stage image with the command linked to its C++ runtime, run as a non-root user | Small, and the same everywhere it runs |
| Prediction markets | One YES book priced 1 to 99, resolving to 0 or 100; no separate NO book | A YES bid is a NO offer: one book trades the same way, without the machinery of complementary shares |
| A prediction's true value | The probability that a hidden walk with news ends above zero | A fair price by construction, which converges as resolution nears, so prices can be checked for calibration |
| Calibration data | Free for commercial use: Polymarket's trades from its public blockchain first | Data a product built on crowdbook can use; exchange feeds' terms limit them to personal or research use |
| What traders know | Beliefs: the value late by a delay of each trader's own, a lasting error, a group's bias, fresh noise; crowds of experts and followers in the examples | Traders who saw the value with fresh noise each look knew it exactly between them, which made prices calibrated by construction; real traders are late, wrong for a while and sometimes partisan |
| Informed capacity | Many traders on ordinary limits rather than a few with huge ones | Real capital is spread over many traders, and more of them act the further the price strays |
| When something fails | Every structure stays valid when an allocation fails or an agent throws; the market stops and refuses to go on; room is made before an order trades, and price levels and nodes are reused | A hosted service can drop one failed market and keep the rest, and nothing a failure leaves behind is undefined; reusing memory made the safety free and cut allocations |
| Depth in memory | Immutable levels shared by every copy of an update | Copying the levels into every event, delivery and snapshot took a quarter of a market's time and most of its allocations |
| Performance guards | Allocation counts in CI, timings against a baseline on one machine | Counts are the same on any machine and catch most slowdowns; timings catch the rest but only mean something where the baseline was recorded |
| Order of work | Gateway, scoring and session reports before more realism | Practice, assessment and testing all need outside participants and a result; realism work is then measured on the markets people use |
| Zero-intelligence cancellation | Each resting order has its own exponential lifetime | A fixed rate per trader let the book grow without limit and pinned the price |
| Timers | Agents on a fixed timer start it at a random point in the first interval | Agents started together otherwise act in lockstep for the whole run |
| Market memory | Noise traders who respond to recent activity and volatility, measured from their own snapshots | Feedback on volatility through liquidity produced long-lived clustering where feedback on activity alone did not, and snapshots keep it cheap for a thousand traders |
| Adaptive traders | A target position set by the chosen strategy, not an order per decision | Ordering every decision filled their limits within a minute; Brock and Hommes model demand as a position |
| Post-only | A time in force, rejected if it would trade | Several exchanges model it this way ("good till crossing"), and it needs no new order field |
| Fee units | Thousandths of a tick-lot, kept apart from cash | Real fees are fractions of a tick; a separate integer keeps accounting exact without shrinking the price range |
| Parent orders | An optional parent id on a new order, which the exchange ignores and the log keeps | An analysis can rebuild every parent from the log, as FIX's linked order ids allow; agents need nothing new |
| VWAP | Comes with the trading day, not with TWAP and POV | It follows a forecast of the day's volume curve; until the market has a day, it is TWAP |
| Auctions | Call auctions: limit orders rest, the book uncrosses at one price that trades the most lots | How real exchanges open, close and restart after halts |
| Uncross ties | Smallest imbalance, then nearest the reference, then lowest | The common exchange rule, made total so the price is always determined |
| Orders in auctions | Market, immediate-or-cancel and post-only rejected | None can wait for the call; rejecting them is plainer than converting them |
| Indicative price | Published during auctions | Agents and people see where the auction is heading, as on real exchanges |
| Halts | A continuous trade outside a band around the last auction price starts an auction; the trade stands | A circuit breaker in its simplest form; stopping trades at the band would need a rule per order type |
| Execution in auctions | Parents pause, children wait | Giving up a parent because the market is in an auction would make brokers walk away twice a day |
| Live play | A pacer around runUntil plus Simulation::act, on one thread | Wall-clock time only decides when a person's actions happen, so a recording replays exactly |
| Trading screen | Terminal, with ANSI escape codes and no library | Runs anywhere with a terminal and adds no dependency; a browser screen is part of the hosted product |
| Session files | TOML holding the scenario's text, the seed and every request with its time | A session replays even when its scenario file changes or is gone |
| Depth feed | Optional, published as events when the best levels change | Agents, the analysis and a trading screen need depth; computing it costs about half again the run time, so markets that do not need it do not pay |
| Analysis | Python (polars, matplotlib) in its own uv project, driving the command line | The C++ build keeps no analysis dependencies, and the analysis uses only what any user gets |
| Long runs | Prices sampled at fixed times, or a log filtered by row kind | The full log of the thousand-trader market comes to about 12 GB per simulated day |

## Milestones

| # | Deliverable | Verified by | Status |
|---|---|---|---|
| M0 | CMake + Ninja presets, GoogleTest, CI (macOS and Linux, sanitizers) | CI green | Done |
| M1 | Order book, matching and self-trade prevention | Differential tests against a reference book; benchmarks | Done |
| M2 | Exchange: validation, order ids, ownership, accounting, risk limits | Conservation of cash and shares; agents' event ledgers match the exchange | Done |
| M3 | Kernel, latency, random streams, agent API, ledger, CSV event log | Same seed gives a byte-identical event log; ledgers match the exchange | Done |
| M4 | Built-in agents (zero-intelligence, Avellaneda–Stoikov market maker, momentum, informed), fundamental value, TOML scenarios, `crowdbook` CLI | Agent tests against a fake context; every example runs in CI | Done |
| M5a | On-demand market data, so crowds of thousands of agents run faster than real time | Scaling benchmark: 10,000 traders at 44× real time | Done |
| M5b | Analysis package; stylized facts, crowd size and price impact by trader type in the thousand-trader markets; market-maker self-impact and PnL against informed flow and latency | [results.md](results.md): four simulated days per market, 32 seeds per experiment point | Done |
| M6 | Depth and order types: the best levels of the book in market data, post-only orders, maker–taker fees | The published depth matches the book after every random request; fees paid add up to fees collected; planted bugs caught | Done |
| M7 | Playable slice: real time at adjustable speed, an outside participant, a terminal price ladder, sessions recorded for replay | A recorded session, scripted and played through the real screen, replays to a byte-identical log | Done |
| M8 | Memory in the crowd: news jumps in the true value, noise traders whose pace follows activity and whose limit orders stand back when prices jump, traders who switch between value and trend strategies by their track records | Volatility clustering at one minute that lasts hours, fat one-minute tails, and ablations naming the cause, in [results.md](results.md) | Done |
| M9 | Large orders worked over time: execution agents slicing parent orders (TWAP and percentage of volume), parent ids in the log | Long memory in the signs of market orders; how the impact of parent orders grows with their size, in [results.md](results.md) | Done |
| M10 | Gateway: a network protocol for orders and market data, its messages kept apart from their encoding (JSON lines first); `crowdbook serve`, a market with seats for several people and bots at once; every arrival recorded, so a session with many participants still replays exactly; input treated as untrusted, with size and rate limits; the terminal screen as a client over the network | A bot and the terminal screen trade in one served market, and its recording replays byte for byte; the parser survives randomized malformed input; planted bugs caught | Done |
| M11 | Python client and example bots: a package installable with pip, using only the standard library; a market maker and a momentum bot as examples | A Python bot trades under the same limits, latency and fees as built-in agents, in CI | Done |
| M12 | Scoring and challenges: a scoring section in scenarios (PnL, risk-adjusted PnL, slippage against a benchmark, inventory and loss limits), computed by the engine; challenges with briefings (make markets within a risk limit, work a large order, trade the news, find the informed flow) | A score recomputed from the session's replay equals the live one; the example bots play every challenge in CI | Done |
| M13 | Session reports, truth and counterfactuals: a JSON report after each session (fills, PnL over time, score, who you traded with and what they knew); the session replayed without your orders; rewind to any moment and trade again | A replay without the participant's orders matches the same seed run without a participant, byte for byte | Done |
| M14 | Engine as a library: a stable API to create, step, feed and inspect a market; version numbers on the protocol, scenario files and session files, with old session files still replaying; a container image; markets per core at real-time speed measured | Session files from earlier versions replay in CI; a capacity benchmark | Done |
| M15 | Hosted product, built on the engine: trading screen in the browser (price ladder with click-to-trade, chart, trade tape, position and PnL, the session report), multiplayer markets hosted online, tournaments and leaderboards | A full session played by hand in the browser, with its report | Planned (hosted product) |
| M16 | Trading day: session schedule, opening and closing auctions, halts, intraday activity pattern, VWAP execution against the day's volume curve; challenges that use them | Auction prices match a naive reference; intraday curves of volume, volatility and spread | Done |
| M17 | Prediction markets: a market type on a yes-or-no question, priced 1 to 99, whose true value is the probability of yes and which resolves to 0 or 100; news that moves it; scenarios and a challenge | Prices of informed markets are calibrated probabilities across many runs; volatility rises toward resolution; the value's process checked against its closed form | Done |
| M18 | Calibration: the same statistics on real data, Polymarket's trades on its public blockchain first, and parameters fitted to match them | A realism scorecard in results.md, real against simulated | Next |
| M19 | Industry protocols: a binary order-entry and market-data encoding of the gateway's messages, and FIX order entry, so trading systems can use crowdbook as a test exchange | A standard FIX client trades through it; both encodings give the same event log as JSON for the same session | Planned |
| M20 | Learning environment: reset and step a market from Python as fast as it can run, many seeds at once, rewards from the scoring rules | A learning agent's runs reproduce from their seeds; throughput benchmark | Planned |
| M21 | Market-design lab: experiments on tick size, fees, speed bumps and circuit breakers | Results in results.md, each with its ablations and uncertainties | Planned |
| M22 | Multiple instruments and futures: an instrument on every order, event, position and log row; futures settled in cash at expiry; arbitrageurs linking future and stock | Cash, shares and contracts conserved across instruments; the future converges to the stock at expiry | Planned |
| M23 | Options: a chain of calls and puts settled in cash, pricing and Greeks, option market makers hedging in the stock, risk limits by delta and vega | Prices and Greeks match closed forms and finite differences; conservation across the chain | Planned |
| M24 | Options research and challenges: whether a volatility smile emerges from supply and demand, dealers' hedging feeding back into the stock, pinning at expiry; an options market-making challenge | Results in results.md; the challenge playable through the gateway | Planned |
| Later | One stock on several exchanges, ETFs and their constituents, rule-based agents | | |

The order puts outside participants, scores and reports before more realism: every use above
needs people and programs from outside to trade and get a result, and the realism work that
follows (M16, M18) is then measured on the markets people actually use.

## Code conventions

- C++23. Public headers in `include/crowdbook/`, sources in `src/`, `#pragma once`.
- Types are `PascalCase`; functions and variables are `camelCase`; private data members end in
  `_`, which keeps `-Wshadow` quiet without renaming constructor parameters. Plain structs have
  public fields without the suffix.
- Warnings are strict (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow` and more) and are errors
  in every preset.
- Aggregate members get default initializers (`{}` if nothing else), so designated initializers
  can skip them without tripping GCC's `-Wmissing-field-initializers`.
- Random draws go into local variables before they are passed as arguments, because argument
  evaluation order is unspecified.
- Formatting is defined in `.clang-format`.
- Code on the hot path follows the rules under [Performance](#performance). A new feature gets a
  benchmark, and the allocation tests and `tools/bench_check.py` must pass before it is merged.
