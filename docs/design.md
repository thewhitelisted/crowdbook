# crowdbook design

## Goal

Simulate a continuous double-auction market as the sum of individual agents, so that market
microstructure — spreads, depth, price impact, volatility — emerges from agent behaviour instead
of being assumed. Anyone should be able to add an agent by writing a C++ class against a small
API and run it against other agents in a reproducible scenario.

Out of scope for the first release: multiple instruments or venues, connectivity to real
exchanges, real-time execution, and a GUI.

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
- **Market data** — anonymous trade prints and top-of-book updates, delivered with each agent's
  own latency, so every agent acts on a slightly stale view, as in real markets. Depth snapshots
  are planned.
- **Scenarios** — a TOML file choosing the agent population and parameters, so experiments run
  without recompiling. Agents register under a name, so user-defined agents work in scenarios too.
- **Event log** — every request the exchange receives and every event it produces, written as CSV
  and analysed in Python (`analysis/`).

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
  `submitMarket`, `cancel`, `modify`, `wakeAt` and `wakeAfter`, plus `now()`, `random()` and
  `ledger()`. The context assigns client order ids, so agents never manage them.
- **Ledger:** each agent's own view of its cash, position and orders, built only from what it sent
  and what it heard back. It shows orders in flight before they are acknowledged, and an event is
  applied to it before the agent's callback for that event runs. Once nothing is in flight it
  matches the exchange exactly.
- **Randomness:** `Random` is xoshiro256** seeded through SplitMix64, with uniform, Bernoulli,
  exponential and normal distributions written here. Agent *n* draws from stream 2*n* and its
  network jitter from stream 2*n* + 1, so turning on jitter or adding another agent does not change
  any agent's own draws. Integer draws are identical on every platform; floating-point draws use
  the same algorithms everywhere but the platform's `log` and `sqrt`.
- **Event log:** `CsvEventLog` writes every request the exchange receives and every event it
  produces, one row each, in processing order. Two runs with the same seed produce byte-identical
  logs.

One trap for agent authors: C++ leaves the evaluation order of function arguments unspecified, so
`context.modify(id, random.uniformInt(...), random.uniformInt(...))` may consume the random stream
in a different order on another compiler. Draw into local variables first.

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
- **Mutation check:** before each randomized suite was committed, deliberately planted bugs were
  each caught. Six in the order book: an off-by-one limit check, LIFO instead of FIFO, no
  self-trade prevention, market orders resting, size increases keeping priority, stale level
  totals. Eight in the exchange: open quantity not released on fills, a flipped seller cash sign, a
  position check ignoring open orders, stale live-order records, unchecked modify increases,
  misreported fill and cancel quantities, missed top-of-book updates. Seven in the kernel: links
  that reorder messages, no same-time tie-break, the ledger updated after the callback, market
  data sent to one agent only, jitter drawn from the agent's own stream, wakeups scheduled in the
  past, start times ignored.
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
| Randomness | xoshiro256** streams from the run's seed: 2*n* for agent *n*'s draws, 2*n* + 1 for its jitter | Same seed → same run; adding an agent or turning on jitter does not change any agent's draws |
| Distributions | Implemented in crowdbook, not `std::*_distribution` | Standard distribution output is implementation-defined, so libc++ and libstdc++ disagree for the same seed |
| Agent state | A ledger built from the agent's own requests and events, not a view of the exchange | Agents act on what they could know, including orders in flight and cancels that race fills |
| Output | Full event log rather than summaries | Microstructure analysis needs every order, cancel and trade, not end-of-run PnL |
| Log format | One CSV, a row per request or event, unused columns empty | Loads into Python in one call, and runs can be compared byte for byte |

## Milestones

| # | Deliverable | Verified by | Status |
|---|---|---|---|
| M0 | CMake + Ninja presets, GoogleTest, CI (macOS and Linux, sanitizers) | CI green | Done |
| M1 | Order book, matching and self-trade prevention | Differential tests against a reference book; benchmarks | Done |
| M2 | Exchange: validation, order ids, ownership, accounting, risk limits | Conservation of cash and shares; agents' event ledgers match the exchange | Done |
| M3 | Kernel, latency, random streams, agent API, ledger, CSV event log | Same seed gives a byte-identical event log; ledgers match the exchange | Done |
| M4 | Built-in agents (zero-intelligence, Avellaneda–Stoikov market maker, momentum, informed), TOML scenarios, `crowdbook` CLI | Example runs | Next |
| M5 | Stylized facts and a first experiment: market-maker PnL vs informed flow and latency | Plots in the README | |
| Later | Rule-based agents, fees, multiple instruments, live viewer | | |

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
