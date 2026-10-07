# crowdbook design

## Goal

crowdbook is a research tool and a game built on one engine: a market realistic enough to study
that you can also trade in yourself.

- **Research.** Simulate a continuous double-auction market as the sum of individual agents, so
  that market microstructure — spreads, depth, price impact, volatility — emerges from agent
  behaviour instead of being assumed. Measure it against real markets, and run experiments no real
  exchange allows: change the tick size, the fees or the speed of the connections, or replay the
  same market without one of its traders.
- **Game.** Trade in the simulated market while it runs, by hand on a trading screen or with your
  own bot, against a crowd of agents. Afterwards, see what a real market never shows: who you
  traded with, which of them knew the true value, and what the market would have done without
  you.
- **Toolkit.** Anyone can add an agent: today a C++ class against a small API, later any program
  that speaks the gateway's protocol. Every run is reproducible from its seed and its recorded
  inputs.

"Realistic" has a finish line: a scorecard of statistics measured the same way on the simulated
market and on a real one (M11).

Out of scope: connecting to real exchanges or trading real money, and multiple instruments or
venues until the single-instrument market is calibrated.

This repository is the open engine, under the MIT license: the simulator, its agents, the
analysis, the terminal trading screen, and, to come, the gateway's protocol and its Python client.
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

On an Apple M5 (Release build, `crowdbook_bench`) the book handles about 15 million operations per
second on a mixed stream of passive orders, cancels, crossing orders and market orders — roughly
65 ns per operation.

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

- **Pacing:** `Pacer` maps a steady wall clock to simulated time at an adjustable speed, stands
  still while paused, and never goes backwards. Each frame, about thirty a second, the session
  runs the market up to the paced time, draws the screen, and reads a key.
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
  that leans against inventory, requoted on a timer and after every fill. Its fair price is a
  running average of trade prices rather than the mid, because the mid is often its own quotes.
- **Momentum trader:** market orders in the direction of a fast moving average's lead over a slow
  one, within a position limit that counts orders in flight.
- **Informed trader:** observes the fundamental value with noise and trades with
  immediate-or-cancel orders priced to keep its edge, so it never sweeps the book past its
  estimate.
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

A scenario names agent groups with shared settings. `AgentRegistry` maps type names to factories
that read a `Parameters` object; after a factory runs, any parameter it did not read is an error,
so a misspelled key cannot silently fall back to a default. `runScenario` builds the market from
a `Scenario` struct and reports trades, volume, the last price, the fundamental's final value and
each group's position, cash and PnL. Scenario files are only one way to fill in that struct: the
TOML reader lives in its own library, so the core library stays free of dependencies.

Running the examples and analysing the results taught five things worth keeping.

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
  the replayed demo session's log, must equal committed values. CI runs it on macOS arm64 and on
  Linux x86-64, so the two platforms have to agree byte for byte. `crowdbook::math` is checked
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
  ids unchecked, a pacer that ignores pause), five in the adaptive trader and three in the
  noise traders' responses were each caught.
- **Session tests** play a scripted session the way a live one runs, in uneven steps with requests
  at chosen nanoseconds, then write it, read it back and replay it: the event log must come out
  byte for byte the same. The real `play` command was also driven through a pseudo-terminal with
  scripted key presses, and its live log and the replay of its recording matched byte for byte.
  The ladder's drawing is tested on fixed screens, with and without colors.
- **Analysis tests** check each statistic in `analysis/` on inputs with known answers: a random
  walk's flat volatility signature, a normal sample's zero excess kurtosis, hand-computed spreads
  and price moves.
- **Sanitizers:** the `asan` preset runs everything under AddressSanitizer and
  UndefinedBehaviorSanitizer, locally and in CI.

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
| Zero-intelligence cancellation | Each resting order has its own exponential lifetime | A fixed rate per trader let the book grow without limit and pinned the price |
| Timers | Agents on a fixed timer start it at a random point in the first interval | Agents started together otherwise act in lockstep for the whole run |
| Market memory | Noise traders who respond to recent activity and volatility, measured from their own snapshots | Feedback on volatility through liquidity produced long-lived clustering where feedback on activity alone did not, and snapshots keep it cheap for a thousand traders |
| Adaptive traders | A target position set by the chosen strategy, not an order per decision | Ordering every decision filled their limits within a minute; Brock and Hommes model demand as a position |
| Post-only | A time in force, rejected if it would trade | Several exchanges model it this way ("good till crossing"), and it needs no new order field |
| Fee units | Thousandths of a tick-lot, kept apart from cash | Real fees are fractions of a tick; a separate integer keeps accounting exact without shrinking the price range |
| Parent orders | An optional parent id on a new order, which the exchange ignores and the log keeps | An analysis can rebuild every parent from the log, as FIX's linked order ids allow; agents need nothing new |
| VWAP | Comes with the trading day, not with TWAP and POV | It follows a forecast of the day's volume curve; until the market has a day, it is TWAP |
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
| M10 | Trading day: session schedule, opening and closing auctions, halts, intraday activity pattern, VWAP execution against the day's volume curve | Auction prices match a naive reference; intraday curves of volume, volatility and spread | Next |
| M11 | Calibration: the same statistics on real order-book data, and parameters fitted to match them | A realism scorecard in results.md, real against simulated | Planned |
| M12 | Market-design lab: experiments on tick size, fees, speed bumps and circuit breakers | Results in results.md, each with its ablations and uncertainties | Planned |
| M13 | Multiple instruments and futures: an instrument on every order, event, position and log row; futures settled in cash at expiry; arbitrageurs linking future and stock | Cash, shares and contracts conserved across instruments; the future converges to the stock at expiry | Planned |
| M14 | Gateway: one network protocol for market data and orders, used by humans and bots alike; a Python client | A Python bot trades through it under the same limits, latency and fees as built-in agents | Planned |
| M15 | Trading screen in the browser, part of the hosted product: price ladder with click-to-trade, chart, trade tape, position and PnL, and a report after each session | A full session played by hand, with its report | Planned (hosted product) |
| M16 | Truth and counterfactuals: after a session, who you traded with and what they knew; the session replayed without your orders; rewind and re-trade | A replay without the participant's orders matches the same seed run without a participant, byte for byte | Planned |
| M17 | Options: a chain of calls and puts settled in cash, pricing and Greeks, option market makers hedging in the stock, risk limits by delta and vega | Prices and Greeks match closed forms and finite differences; conservation across the chain | Planned |
| M18 | Options research and game: whether a volatility smile emerges from supply and demand, dealers' hedging feeding back into the stock, pinning at expiry; an options market-making challenge | Results in results.md; the challenge playable on the trading screen | Planned |
| M19 | Challenges and tournaments: scored scenarios (work a large order against VWAP, make markets within a risk limit, trade the news); bot tournaments and leaderboards are part of the hosted product | Scenarios with published scoring, played by the example bots | Planned (tournaments and leaderboards: hosted product) |
| Later | Multiplayer markets hosted online (hosted product), one stock on several exchanges, ETFs and their constituents, an environment for training learning agents, rule-based agents | | |

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
