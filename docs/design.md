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
- **Accounts:** cash (in tick-lots) and position. Cash has no limit and may go negative, so risk is
  bounded by position alone. Each trade moves `price × quantity` of cash from buyer to seller and
  `quantity` shares the other way, so total cash and total shares never change.
- **Events** for each request are appended in a fixed order, in the style of FIX execution reports:
  1. the sender's `OrderAccepted`, `OrderModified`, `OrderCancelled` or `OrderRejected`;
  2. for each execution, the maker's `OrderFilled`, the taker's `OrderFilled` and a public `Trade`;
  3. `OrderCancelled` for quantity that could not rest (immediate-or-cancel or market remainder,
     self-trade prevention);
  4. `TopOfBook`, if the best bid or ask changed in price or size.

  Every request gets at least one event, and the first always answers the sender. Fills carry the
  order's remaining open quantity, so an agent can track its own orders and balances from its
  events alone.
- **Market data:** `Trade` and `TopOfBook` are public and never identify agents or orders.

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
  any agent's own draws. Integer draws are identical on every platform; floating-point draws use
  the same algorithms everywhere but the platform's `log` and `sqrt`.
- **Market data:** an agent chooses how it gets public data by overriding `marketData()`.
  Streaming agents receive every trade and top-of-book change through `onTrade` and
  `onTopOfBook`. Snapshot agents receive nothing and call `context.market()` when they act, which
  returns what the exchange had published one `fromExchange` latency earlier: never anything they
  could not have seen yet. Streaming every update to every agent costs N² as the crowd grows, so
  the built-in traders that only look at the market when they act use snapshots; the market
  maker, which reacts to every trade, streams. The kernel keeps a short history of public states
  for snapshot reads and drops each state once no agent's latency can reach back to it, so memory
  does not grow with the length of a run.
- **Event log:** `CsvEventLog` writes every request the exchange receives and every event it
  produces, one row each, in processing order, optionally only rows of chosen kinds. Two runs with
  the same seed produce byte-identical logs. `PriceSampler` writes the best bid, best ask and last
  trade price at fixed times instead, which is all that return statistics over long runs need, and
  `BroadcastSink` feeds one run to several sinks.

One trap for agent authors: C++ leaves the evaluation order of function arguments unspecified, so
`context.modify(id, random.uniformInt(...), random.uniformInt(...))` may consume the random stream
in a different order on another compiler. Draw into local variables first.

## Agents and scenarios

Four agent types are built in. Each is a plain class configured by a struct, so it can be used
from C++ directly; [scenarios.md](scenarios.md) lists their parameters.

- **Zero-intelligence trader** (Farmer, Patelli and Zovko, 2005): limit and market orders at
  Poisson times, on random sides, with limit prices drawn inside the opposite best quote. Each
  resting order is cancelled after its own exponentially distributed lifetime.
- **Market maker** (Avellaneda and Stoikov, 2008): one bid and one ask around a reservation price
  that leans against inventory, requoted on a timer and after every fill. Its fair price is a
  running average of trade prices rather than the mid, because the mid is often its own quotes.
- **Momentum trader:** market orders in the direction of a fast moving average's lead over a slow
  one, within a position limit that counts orders in flight.
- **Informed trader:** observes the fundamental value with noise and trades with
  immediate-or-cancel orders priced to keep its edge, so it never sweeps the book past its
  estimate.

The fundamental value is an Ornstein–Uhlenbeck process (a random walk when its mean reversion is
zero, the default) stepped with its exact discrete-time formulas from its own random stream, so its path does not
depend on who reads it or when.

A scenario names agent groups with shared settings. `AgentRegistry` maps type names to factories
that read a `Parameters` object; after a factory runs, any parameter it did not read is an error,
so a misspelled key cannot silently fall back to a default. `runScenario` builds the market from
a `Scenario` struct and reports trades, volume, the last price, the fundamental's final value and
each group's position, cash and PnL. Scenario files are only one way to fill in that struct: the
TOML reader lives in its own library, so the core library stays free of dependencies.

Running the examples and analysing the results taught four things worth keeping.

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

## Analysis

`analysis/` is a Python project, managed with [uv](https://docs.astral.sh/uv/), that drives the
Release build of `crowdbook` through its command line and reads its CSV and JSON output, the same
interface any user has. Nothing in the C++ build depends on it.

- `log` reads event logs and price samples into [polars](https://pola.rs) data frames, and pairs
  each execution's maker with its taker.
- `facts` holds the statistics: returns over a horizon, excess kurtosis, autocorrelation, tail
  distributions, volatility at each horizon (the signature plot), the time-weighted spread
  distribution, the mid's move after aggressive orders by group and horizon, and markouts of
  each execution for the agent whose resting order it filled.
- `runner` writes a scenario, runs it, and runs many in parallel.
- `style` gives every chart the same thin marks and recessive axes, with series colors checked as a
  set for color-vision deficiencies.
- `crowdbook-facts` measures stylized facts of the large example markets, and `crowdbook-experiments`
  runs the market-maker experiments. Both print their tables as Markdown and save their charts to
  `docs/images`; [results.md](results.md) reports what they found.

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-facts
uv run --project analysis crowdbook-experiments
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
  invalid ones and some from an agent with no account, for 30 seeds of 2,000 steps. After every
  step:
  - `Exchange::audit()` cross-checks accounts, live orders and the book, and checks that cash and
    shares are conserved and no position limit can be breached;
  - the public feed must agree with the book;
  - each agent's ledger, rebuilt only from its own events, must match its account and open orders
    exactly.
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
  past, start times ignored.
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
| Prices and sizes | `int64` ticks and lots; cash in tick-lots | Exact arithmetic; no floating point in matching or accounting |
| Priority | Price, then time | Standard for continuous limit order books |
| Modify | Size decrease keeps queue position; price change or size increase loses it | Matches common exchange rules, so queue-position effects are realistic |
| Self-trade prevention | Cancel the incoming order's remaining size | An agent never trades with itself and the book never ends up crossed |
| Market orders | Fill against the book, cancel any remainder | Market orders never rest |
| Links | First in, first out per agent and direction, even with jitter | Like a TCP connection: a cancel can never overtake the order it cancels |
| Market data delivery | Streamed or read on demand, chosen by each agent | Streaming to everyone costs N²; most agents only need the market at the moment they act |
| Randomness | xoshiro256** streams from the run's seed: 2*n* for agent *n*'s draws, 2*n* + 1 for its jitter | Same seed → same run; adding an agent or turning on jitter does not change any agent's draws |
| Distributions | Implemented in crowdbook, not `std::*_distribution` | Standard distribution output is implementation-defined, so libc++ and libstdc++ disagree for the same seed |
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
| M5a | On-demand market data, so crowds of thousands of agents run faster than real time | Scaling benchmark: 10,000 traders at 52× real time | Done |
| M5b | Analysis package; stylized facts, crowd size and price impact by trader type in the thousand-trader markets; market-maker self-impact and PnL against informed flow and latency | [results.md](results.md): four simulated days per market, 32 seeds per experiment point | Done |
| M6 | Depth and order types: the best levels of the book in market data, post-only orders, maker–taker fees | Depth matches the reference book in differential tests; cash plus fees is conserved | Next |
| M7 | Playable slice: real time at adjustable speed, an outside participant whose orders arrive through a queue, a bare price ladder, sessions recorded for replay | A recorded session replays to a byte-identical log | Planned |
| M8 | Memory in the crowd: news jumps in the true value, self-exciting activity (Hawkes processes), traders who switch between value and trend strategies by recent PnL | Volatility clustering at one minute that lasts hours; fat one-minute tails; ablations name the cause | Planned |
| M9 | Large orders worked over time: execution agents slicing parent orders (TWAP, VWAP, percentage of volume) | Long memory in the signs of market orders; square-root impact of parent orders | Planned |
| M10 | Trading day: session schedule, opening and closing auctions, halts, intraday activity pattern | Auction prices match a naive reference; intraday curves of volume, volatility and spread | Planned |
| M11 | Calibration: the same statistics on real order-book data, and parameters fitted to match them | A realism scorecard in results.md, real against simulated | Planned |
| M12 | Gateway: one network protocol for market data and orders, used by humans and bots alike; a Python client | A Python bot trades through it under the same limits, latency and fees as built-in agents | Planned |
| M13 | Trading screen: price ladder with click-to-trade, chart, trade tape, position and PnL, and a report after each session | A full session played by hand, with its report | Planned |
| M14 | Truth and counterfactuals: after a session, who you traded with and what they knew; the session replayed without your orders; rewind and re-trade | A replay without the participant's orders matches the same seed run without a participant, byte for byte | Planned |
| M15 | Market-design lab: experiments on tick size, fees, speed bumps and circuit breakers | Results in results.md, each with its ablations and uncertainties | Planned |
| M16 | Challenges and tournaments: scored scenarios (work a large order against VWAP, make markets within a risk limit, trade the news), bot tournaments, leaderboards | Scenarios with published scoring; a tournament of the example bots | Planned |
| Later | Multiplayer markets hosted online, multiple instruments, an environment for training learning agents, rule-based agents | | |

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
