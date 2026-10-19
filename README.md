# crowdbook

An agent-based limit order book simulator in C++23: a research tool for market microstructure,
and a market you can trade in yourself, by hand or with your own bot, that shows you afterwards who
you traded with and what they knew.

crowdbook models a market as a crowd of individual traders — market makers, informed traders,
trend followers, noise traders — each sending orders to a simulated exchange over its own network
link. Spreads, depth, price impact and volatility are not assumed; they emerge from how the agents
interact. Writing your own agent means writing one C++ class and naming it in a scenario file,
or writing a program in any language that trades over the network.

It fits itself to a recorded market, says whether the fit can be trusted for testing strategies,
and then tests them: [docs/tutorial.md](docs/tutorial.md) goes from a recording to a verdict and a
tested strategy in about fifteen minutes.

> **Status:** the engine (order book, exchange, simulation kernel, built-in agents, scenario
> files), trading in it by hand or over the network, session reports, the learning environment,
> and fitting to recordings with the checks on a fit are done; [docs/results.md](docs/results.md)
> reports the experiments, and [docs/design.md](docs/design.md#roadmap) has the roadmap.

## Install

Python 3.12 or later, on Linux (x86-64 or arm64, glibc 2.28 or later) or macOS 14 or later on
Apple silicon, from the latest release's ready-built wheels on PyPI, with the repository at that
release for the examples:

```bash
git clone --branch v0.13.0 https://github.com/thewhitelisted/crowdbook && cd crowdbook
python3 -m venv .venv && . .venv/bin/activate
pip install crowdbook-engine==0.13.0 crowdbook-analysis==0.13.0
```

That installs the `crowdbook` command and the learning pool `crowdbook_learn` (the
`crowdbook-engine` package), and the analysis commands, `crowdbook-assess`,
`crowdbook-calibrate`, `crowdbook-validate` and the rest (`crowdbook-analysis`), in under a
minute. On any other platform the same command builds the engine from source, as does
`pip install . ./analysis` from a checkout of `main`, with CMake 3.25+ and a C++23 compiler
([docs/quickstart.md](docs/quickstart.md#install));
[Building](#building) covers the C++ build with its tests.

## Quick start

```bash
crowdbook run examples/scenarios/market_maker.toml --log run.csv
```

```
examples/scenarios/market_maker.toml (seed 7): simulated 60s in 0.05s
1842 trades, 6338 lots traded, last price 9924

group                 agents   position           cash          pnl
maker                      1          0           1575        +1575
noise                     20          0          -1575        -1575
```

A market maker with a fast connection earns the spread from twenty zero-intelligence traders.
`run.csv` holds every order, cancel, fill, trade and top-of-book change. The same seed always
reproduces the same run, byte for byte, on any platform.
[examples/scenarios](examples/scenarios) has fourteen scenarios, from this one to thousand-trader
markets with informed traders, memory, or brokers working large orders, a trading day, a
prediction market, one fitted to Polymarket's fifteen-minute Bitcoin questions and a contract on
a game, whose probability jumps when either side scores;
[docs/scenarios.md](docs/scenarios.md) describes the file format and every agent parameter.

Where to go from here:

| To | Read |
|---|---|
| Fit a recorded market, check the fit, and test a strategy in it | [docs/tutorial.md](docs/tutorial.md), then [docs/validation.md](docs/validation.md) |
| Run, play and serve markets; write a bot or a learning agent | [docs/quickstart.md](docs/quickstart.md) |
| Write a scenario | [docs/scenarios.md](docs/scenarios.md) |
| Step markets from Python for learning | [python/README.md](python/README.md) |
| Trade from Python over the network | [clients/python/README.md](clients/python/README.md), [docs/protocol.md](docs/protocol.md) |
| Use the engine from C++ | [docs/library.md](docs/library.md) |
| See what the simulated markets show | [docs/results.md](docs/results.md) |
| Understand how it works and is tested | [docs/design.md](docs/design.md) |

## Results

A day of a 1,432-agent market — a thousand noise traders, two market makers, twenty trend
followers, and ten experts and four hundred followers who see the asset's true value late, each
with errors of its own — simulates in 35 seconds. Measured over four such days and thousands of
smaller markets ([docs/results.md](docs/results.md)):

- **Who moves prices.** Noise traders' orders move the mid a tenth of a tick, and the move is gone
  within a second. Orders from traders who follow the value move it one and a half to two ticks,
  for good. Trend followers' orders arrive as moves end, and the price reverses two ticks against
  them, which costs them 3.4 ticks on every lot.
- **Headcount is not what matters.** The same order flow from 100 or 1,000 noise traders gives the
  same market. Fat tails and volatility clustering appear only when agents react to the market.
- **Liquidity feedback makes volatility cluster.** When noise traders stand back from the book
  after prices jump, the thinner book makes the next jump bigger: volatility clusters for an hour
  or two, and one-minute returns have fat tails. A crowd of value traders who trade small
  mispricings undoes it: between them they are a deep book that does not thin.
- **Large orders leave two marks.** Brokers working parent orders with a Pareto tail of sizes give
  the signs of market orders the long memory that theory predicts from that tail. A parent's
  impact grows almost linearly with its size in a book that forgets in seconds, and bends toward
  the square root of real markets (an exponent of 0.6) when resting orders last 50 seconds.
- **Informed traders barely cost the market maker.** Its fills against them lose about 0.9 ticks
  per lot. But by pulling prices back to value they make its fills against noise traders worth
  one to two ticks more, and the noise traders pay for both.
- **Late traders give prediction markets a favourite–longshot bias.** In a market on a yes-or-no
  question, of the moments the price said 70 to 80 cents, three quarters resolved yes, but of
  those at 10 to 20, only 6%: traders who see the news a minute late hold prices back from 0 and
  100, as in real prediction and betting markets, and trend followers profit from the
  underreaction. Prices move fastest in the last minutes of questions still open, as on election
  night.
- **Real prediction markets are fast and calibrated.** Over two weeks of Polymarket's questions on
  whether Bitcoin will be up in five minutes, fifteen or an hour, 8.2 million orders read from the
  chain, prices follow Bitcoin within a block or two and show no favourite–longshot bias; order
  sizes have a power-law tail and order signs remember. A market fitted to the fifteen-minute
  questions matches most of the scorecard, where the prediction example's late crowd does not,
  and its takers pay Polymarket's fee, which every real fill shows is exactly 0.07·q·(1 − q)
  dollars a share.
- **What's missing.** Volatility clusters for an hour, not the weeks of real markets; the fitted
  market prices cheap and dear shares with a favourite–longshot bias the real ones do not have,
  trades too often and does not crowd in at the end as real traders do, and nothing is fitted to
  a real order book yet.

![Mid move after the aggressive orders of noise traders, trend followers, experts and followers](docs/images/impact.png)

The analysis is a separate Python package that drives the `crowdbook` command:

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-facts
uv run --project analysis crowdbook-experiments
uv run --project analysis crowdbook-large-orders
uv run --project analysis crowdbook-trading-day
uv run --project analysis crowdbook-prediction
uv run --project analysis crowdbook-polymarket --from 2026-09-28 --to 2026-10-12
uv run --project analysis crowdbook-realism
```

Any market, recorded or simulated, can be held against another: [docs/validation.md](docs/validation.md)
describes the format both are read in, a report of the same statistics on each with intervals and
distances, a calibration that fits any scenario to a recording and checks that it can find
settings it knows, a check that the fitted market pays no strategy, generic or learned, the
recording does not, and a generator of many labelled markets for training and analysis. From a recording to a
verdict on whether its fit can be trusted for testing strategies, in one command, as
[docs/tutorial.md](docs/tutorial.md) shows step by step:

```bash
crowdbook-assess examples/tutorial/spec.toml --real RECORDED_MARKETS --output assessed
```

## Trade in it yourself

`crowdbook play` runs a scenario in real time on a trading screen in the terminal, with you as
one more trader:

```bash
crowdbook play examples/scenarios/playable.toml --record session.toml
```

```
crowdbook  00:11.6 of 01:00.0  speed 5x  running
position +5  cash -50012  fees 1.0  pnl +4.5 at 10003.5

  your bids     bids    price     asks your asks
                        10008        46
                        10007        53
                        10006        64
                        10005        95
>                       10004        97         7
                   1    10003*
                  15    10002
                  50    10001
                  78    10000
                  53     9999

trades:  10003 x3 down  10004 x7 up  10004 x8 up  10004 x2 up  10004 x4 up
arrows price  m mid  b bid  s offer  B buy now  S sell now  size 7 (+/-)
c cancel here  C cancel all  [ ] speed  space pause  q quit
offer 7 at 10004
```

The arrow keys move the price you trade at. `b` and `s` bid and offer there, `B` and `S` buy and
sell at the market, and `c` and `C` cancel. `[` and `]` change the speed, and space pauses. Your
orders travel like everyone else's: a millisecond each way, under the exchange's position limits
and fees.

With `--record`, the session can be replayed exactly. The replay produces the same event log,
byte for byte, so a played session becomes data to analyse:

```bash
crowdbook replay session.toml --log session.csv
```

## Serve it to people and bots

`crowdbook serve` runs a market in real time for several traders at once, over TCP. Each seat is
one participant, with the scenario's account and latency, and anything that speaks the
[protocol](docs/protocol.md), JSON with one message per line, can claim it:

```bash
crowdbook serve examples/scenarios/playable.toml --seat alice --seat bob \
    --record session.toml
```

The clock starts once every seat is claimed. A person can trade from the terminal screen:

```bash
crowdbook connect 127.0.0.1:7878 --seat alice
```

and a program needs only a socket. In Python:

```python
import json, socket

conn = socket.create_connection(("127.0.0.1", 7878)).makefile("rw")
def send(message):
    conn.write(json.dumps(message) + "\n")
    conn.flush()

send({"type": "hello", "protocol": 1, "seat": "bob"})
send({"type": "new", "id": 1, "side": "buy", "order_type": "market", "quantity": 2})
for line in conn:
    print(json.loads(line))  # welcome, start, the order's events, trades, quotes...
```

The Python client in [clients/python](clients/python), standard library only, does the
bookkeeping: a bot overrides callbacks and acts through methods, much as an agent inside the
market does.

```python
import crowdbook

class DipBuyer(crowdbook.Bot):
    def on_trade(self, trade):
        if trade.price < self.settings.reference_price - 5 and self.ledger.position < 10:
            self.buy_market(1)

crowdbook.run(DipBuyer(), port=7878, seat="bob")
```

Clients name orders with their own ids and can cancel an order before it is acknowledged. A
dropped connection cancels its seat's orders. The server checks every message and limits how fast
each connection may send, and it listens only on 127.0.0.1 unless `--listen` says otherwise. The
recording replays the whole session, every seat's orders included, to the same event log byte for
byte.

## After the session: what you could not see

`crowdbook report` replays a recorded session and tells each seat what a real market never
would: who was on the other side of every fill, how far the true value was from the price when
they traded, and what the market would have done without you, by replaying it without your
orders.

```bash
crowdbook report examples/sessions/served_demo.toml --json report.json
```

```
seat alice: 23 fills, 42 lots
  traded with      type                fills  bought    sold   their edge   your markout
  noise            zero_intelligence      19      14      21        -1.05          +0.50
  informed         informed                3       6       0        +2.76              -
  bob              participant             1       0       1        -3.65              -
  pnl at the end +24.2, +33.0 at the true value (tick-lots, net of fees)
  without you: last price 10003 instead of 10003; you moved the mid +0.15 ticks on average
```

Alice made her money off the noise traders, and gave almost three ticks a lot to the informed
traders, who knew the true value when they traded with her. `play` and `serve` write the same
report at the end with `--report`. And any session can be rewound to a moment and played on
differently:

```bash
crowdbook play session.toml --at 2m --record retry.toml
```

## Challenges

A scenario can score whoever trades in it and brief them first. Five challenges come with the
project, each a market with a task:

| Challenge | Task | Scored on |
|---|---|---|
| [market_making.toml](examples/challenges/market_making.toml) | Be the only market maker among noise traders and traders who follow the true value | PnL at the true value, less a charge for inventory held; a loss of 2,000 stops you |
| [large_order.toml](examples/challenges/large_order.toml) | Buy 300 lots in ten minutes | Against the market's VWAP; unfinished lots cost 10 each |
| [news.toml](examples/challenges/news.toml) | Trade jumps in the true value you cannot see | PnL at the true value |
| [informed_flow.toml](examples/challenges/informed_flow.toml) | Make markets where two lots in five that take liquidity are informed | PnL at the true value, less a charge for inventory held |
| [prediction.toml](examples/challenges/prediction.toml) | Trade a yes-or-no question that resolves in ten minutes, without seeing the news | PnL at resolution, 100 or 0 a share |

```bash
crowdbook play examples/challenges/large_order.toml
```

The engine computes the score from the exchange's events, so replaying a session's recording
gives the same score, which anyone can check. The example bots play every challenge in CI.

## Writing an agent

An agent reacts to callbacks and acts through its context, which only lets it send requests to the
exchange and schedule wakeups:

```cpp
// Buys one lot when a trade prints more than `band` ticks below the recent average trade price,
// and sells one when a trade prints that far above it.
class MeanReverter final : public crowdbook::Agent {
public:
    MeanReverter(double band, crowdbook::Price referencePrice)
        : band_(band), average_(static_cast<double>(referencePrice)) {}

    void onTrade(crowdbook::AgentContext& context, const crowdbook::Trade& trade) override {
        const auto price = static_cast<double>(trade.price);
        if (price < average_ - band_) {
            context.submitMarket(crowdbook::Side::Buy, 1);
        } else if (price > average_ + band_) {
            context.submitMarket(crowdbook::Side::Sell, 1);
        }
        average_ += 0.05 * (price - average_);
    }

private:
    double band_;
    double average_;
};
```

Register it under a name, and any scenario can use it with `type = "mean_reverter"`:

```cpp
crowdbook::AgentRegistry registry = crowdbook::AgentRegistry::withBuiltIns();
registry.add("mean_reverter", [](const crowdbook::Parameters& parameters,
                                 const crowdbook::Environment& environment) {
    return std::make_unique<MeanReverter>(parameters.number("band", 3.0),
                                          environment.referencePrice);
});
```

[examples/custom_agent.cpp](examples/custom_agent.cpp) is the complete program, with a position
limit, and builds with the project.

## Step it from Python

A learning agent trades through one seat in each of many markets, which a pool steps together on
threads of its own, in C++, while Python waits without holding its lock:

```python
import crowdbook_learn

pool = crowdbook_learn.Pool("examples/learning/market.toml", markets=64)
obs, info = pool.reset(seeds=range(64))
while True:
    actions = policy(obs)  # 64 rows of (action, size)
    obs, rewards, terminated, truncated, info = pool.step(actions)
    if terminated.any():
        pool.reset(seeds=fresh_seeds(), mask=terminated.copy())
```

The seat decides every second of market time and whenever one of its orders fills. It sees what a
trader would have heard since its last decision, with the times it heard it: trades, the best
prices, depth, its own acknowledgements and fills, and a feed of the true value, late and noisy; and
its position, cash, orders and the time left. It quotes, joins, improves, steps back, cancels or
takes, at one of a few sizes, and its orders rest between decisions. The reward is the change in its
score, net of fees, with positions valued at the true value where the market has one and closed
through the book at the end, or valued at resolution in a prediction market, so an episode's rewards
add up to its score. The observations are NumPy arrays over the pool's own memory, never copied.
`crowdbook_learn.VectorEnv` puts a pool behind Gymnasium's vector API.

Every episode is a session with one seat: it runs again the same from its seeds, on any number of
threads and on Linux and macOS alike, and `pool.session(i)` writes it as a file `crowdbook replay`
and `crowdbook report` read. A scenario's `[learning]` section can draw any of its settings at
random for each episode, from ranges ([docs/scenarios.md](docs/scenarios.md#learning-optional)),
and [docs/design.md](docs/design.md#learning-environment) describes the environment.

## Use it as a library

Everything the `crowdbook` command does is a library call: run markets, serve them over your own
transport through the gateway, score and report on sessions. Install a build and
`find_package(crowdbook)`; [docs/library.md](docs/library.md) maps what to use for what, the
version numbers of the formats, the capacity per core, and the container image.

## Building

Each workflow preset configures, builds and runs the test suite, with output in `build/<preset>/`.

| Preset    | Build                                                      |
|-----------|------------------------------------------------------------|
| `dev`     | Debug                                                      |
| `release` | Release, including the benchmarks                          |
| `asan`    | Debug with AddressSanitizer and UndefinedBehaviorSanitizer |

The core library has no dependencies. `serve` waits on its sockets with epoll or kqueue, so it needs
Linux, macOS or a BSD; `connect` needs any POSIX system. Scenario files use
[toml++](https://github.com/marzer/tomlplusplus) and the tests use GoogleTest; CMake fetches both at
pinned versions, and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has toml++'s license, since it
is compiled into the `crowdbook` command. The analysis in `analysis/` is a
[uv](https://docs.astral.sh/uv/) project using polars, NumPy and matplotlib; nothing in the C++
build depends on it. When CMake finds Python 3.12 or later with its development files, the build
also makes the learning module, `crowdbook_learn`, with
[nanobind](https://github.com/wjakob/nanobind) fetched at a pinned version, in
`build/<preset>/python`; put that directory on `PYTHONPATH`. It needs NumPy, and Gymnasium for
`VectorEnv`; `-DCROWDBOOK_BUILD_PYTHON=OFF` leaves it out. THIRD_PARTY_NOTICES.md has the licenses
of nanobind and robin-map too, which are compiled into it.

## Performance

Measured on one core of an Apple M5 with a Release build:

- The matching engine handles about 45 million operations per second (roughly 22 ns each) on a
  mixed stream of passive orders, cancels, crossing orders and market orders.
- A simulated day of the 1,432-agent mixed market in
  [large_market.toml](examples/scenarios/large_market.toml), 17.5 million trades, takes 35
  seconds: about 2,500 times faster than real time.
- The markets of the example challenges run 27,000 to 42,000 times faster than real time, and
  18,000 to 26,000 times when served through the gateway to a client that reads every message.
- A pool of the example learning market steps about 30,000 decisions a second, each a second of
  market time, over 64 markets on four threads of a four-core x86 cloud machine, and 17,000 for one
  market on one thread.
- The event log writes about 280 MB a second, and the server encodes a ten-level depth update in
  about 120 ns.
- Served over TCP to 50 trading clients, each placing or cancelling 20 orders a second and reading
  every market data message, the server answers within 0.15 ms of the market's own latency at the
  median and 0.48 ms at the 99th percentile, on 19% of a core. With 200 clients and market data
  batched every 10 ms, it is 0.18 and 1.2 ms on 31% of a core. A served market takes about 430 KB
  of memory.
- Whole markets of zero-intelligence traders, each acting about four times a second, simulate
  this many seconds per second of wall-clock time:

  | Traders | Every update pushed to every trader | Traders read the market on demand |
  |---:|---:|---:|
  | 100 | 1,051 | 11,665 |
  | 1,000 | 9.6 | 805 |
  | 10,000 | — | 54 |

  Pushing every trade and quote change to every trader costs N² as the crowd grows, so agents
  that only look at the market when they act read it on demand instead. Market makers, which
  react to every trade, still get the full stream.

Speed is guarded: tests count the heap allocations per request on the hot paths and fail CI if
they grow, and `tools/bench_check.py` compares every benchmark with a recorded baseline. To
reproduce:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench
python3 tools/bench_check.py
```

## License

[PolyForm Shield 1.0.0](LICENSE): you may use, change and share crowdbook for any purpose except
providing a product that competes with it, or with a product its author provides using it.
Everything published before this licence, version 0.12.0 included, was released under MIT and
stays under it. The Python client in [clients/python](clients/python) stays under
[MIT](clients/python/LICENSE); the learning module in [python](python) is part of the engine and
under its license. Third-party code compiled into the binary is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
