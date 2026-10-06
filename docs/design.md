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
  sequence number, so simultaneous events always resolve the same way.
- **Exchange** — the only component that changes the book or any account. Validates and
  acknowledges orders, assigns order ids, enforces ownership, applies self-trade prevention and
  risk limits, and keeps each agent's cash and position.
- **OrderBook** — price-time priority matching on integer ticks. Supports limit, market, cancel,
  modify and immediate-or-cancel orders.
- **Agents** — subclasses of `Agent` that react to callbacks (start, market data, order events,
  timer wakeups) and act through a context that can submit, cancel and modify orders, schedule
  wakeups, read the clock and draw random numbers. Agents never touch the book directly and never
  see other agents' identities.
- **Market data** — anonymous top of book, trade prints and depth snapshots, delivered with
  latency, so every agent acts on a slightly stale view, as in real markets.
- **Scenarios** — a TOML file choosing the agent population and parameters, so experiments run
  without recompiling. Agents register under a name, so user-defined agents work in scenarios too.
- **Event log** — every order, acknowledgement, fill and snapshot, written as CSV and analysed in
  Python (`analysis/`).

## Decisions

Choices for later milestones may change once they are implemented; changes are recorded here.

| Topic | Choice | Reason |
|---|---|---|
| Agent access | Messages through the exchange only; no direct access to the book | Makes latency, ownership checks and risk limits possible; no agent can corrupt the book or touch another agent's orders |
| Order ids | Every accepted order is acknowledged with an id; cancel and modify by id | Agents need to manage resting orders to requote, which market making depends on |
| Accounting | Kept by the exchange; total cash and shares are conserved, checked by tests | PnL only means something if no fill is ever lost or double-counted |
| Time | `int64` nanoseconds, discrete events | Latency and queue position matter in microstructure; fixed rounds hide both |
| Ties | Same timestamp → insertion order | Deterministic; no hidden dependence on container iteration order |
| Prices and sizes | `int64` ticks and lots; cash in tick-lots | Exact arithmetic; no floating point in matching or accounting |
| Priority | Price, then time | Standard for continuous limit order books |
| Modify | Size decrease keeps queue position; price change or size increase loses it | Matches common exchange rules, so queue-position effects are realistic |
| Self-trade prevention | Cancel the incoming order's remaining size | An agent never trades with itself and the book never ends up crossed |
| Market orders | Fill against the book, cancel any remainder | Market orders never rest |
| Randomness | Each agent gets its own stream derived from the root seed and its id | Same seed → same run; adding an agent does not change the others' draws |
| Distributions | Implemented in crowdbook, not `std::*_distribution` | Standard distribution output is implementation-defined, so libc++ and libstdc++ disagree for the same seed |
| Output | Full event log rather than summaries | Microstructure analysis needs every order, cancel and trade, not end-of-run PnL |

## Milestones

| # | Deliverable | Verified by |
|---|---|---|
| M0 | CMake + Ninja presets, GoogleTest, CI (macOS and Linux, sanitizers) | CI green |
| M1 | Order book and matching | Random order streams checked against a reference book; throughput benchmark |
| M2 | Exchange: validation, order ids, self-trade prevention, accounting, risk limits | Conservation of cash and shares |
| M3 | Kernel, latency, random streams, agent API | Same seed gives an identical event log |
| M4 | Built-in agents (zero-intelligence, Avellaneda–Stoikov market maker, momentum, informed), TOML scenarios, `crowdbook` CLI | Example runs |
| M5 | Stylized facts and a first experiment: market-maker PnL vs informed flow and latency | Plots in the README |
| Later | Rule-based agents, fees, multiple instruments, live viewer | |

## Code conventions

- C++23. Public headers in `include/crowdbook/`, sources in `src/`, `#pragma once`.
- Types are `PascalCase`; functions and variables are `camelCase`; data members end in `_`, which
  keeps `-Wshadow` quiet without renaming constructor parameters.
- Warnings are strict (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow` and more) and are errors
  in every preset.
- Formatting is defined in `.clang-format`.
