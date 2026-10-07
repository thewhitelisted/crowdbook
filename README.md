# crowdbook

An agent-based limit order book simulator in C++23: a research tool for market microstructure,
and a market you can trade in yourself, by hand or with your own bot, that shows you afterwards who
you traded with and what they knew.

crowdbook models a market as a crowd of individual traders — market makers, informed traders,
trend followers, noise traders — each sending orders to a simulated exchange over its own network
link. Spreads, depth, price impact and volatility are not assumed; they emerge from how the agents
interact. Writing your own agent means writing one C++ class and naming it in a scenario file.

> **Status:** the order book, the exchange, the simulation kernel, six built-in agents, scenario
> files, the `crowdbook` command and the Python analysis package are done, and
> [docs/results.md](docs/results.md) reports the experiments. You can trade in a market yourself,
> in real time, and replay the session exactly afterwards. A market with memory shows volatility
> clustering for about an hour, and brokers working large orders give order flow long memory and
> price impact whose shape depends on how long the book remembers. Next: a network gateway, so
people and bots from outside can trade in the same market. See
> [docs/design.md](docs/design.md) for the goal, the architecture, the testing approach and the
> roadmap.

## Quick start

Requires CMake 3.25+, Ninja and a C++23 compiler (CI covers Apple clang and GCC 14).

```bash
cmake --workflow --preset dev
```

```bash
./build/dev/apps/crowdbook run examples/scenarios/market_maker.toml --log run.csv
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
reproduces the same run, byte for byte, on any platform. [examples/scenarios](examples/scenarios) has ten
scenarios, from this one to thousand-trader markets with informed traders, memory, or brokers
working large orders; [docs/scenarios.md](docs/scenarios.md) describes the file format and every
agent parameter.

## Results

A day of a 1,041-agent market — a thousand noise traders, a market maker, twenty trend followers
and twenty informed traders who know the asset's true value — simulates in 41 seconds. Measured
over four such days and more than a thousand smaller markets ([docs/results.md](docs/results.md)):

- **Who moves prices.** Noise traders' orders move the mid a tenth of a tick, and the move is gone
  within a second. Informed traders' orders move it two ticks, for good. Trend followers' orders
  arrive as moves end, and the price reverses two ticks against them, which costs them 3.3 ticks
  on every lot.
- **Headcount is not what matters.** The same order flow from 100 or 1,000 noise traders gives the
  same market. Fat tails and volatility clustering appear only when agents react to the market.
- **Liquidity feedback makes volatility cluster.** When noise traders stand back from the book
  after prices jump, the thinner book makes the next jump bigger: volatility clusters for about
  an hour, and one-minute returns have fat tails.
- **Large orders leave two marks.** Brokers working parent orders with a Pareto tail of sizes give
  the signs of market orders the long memory that theory predicts from that tail. A parent's
  impact grows almost linearly with its size in a book that forgets in seconds, and bends toward
  the square root of real markets (an exponent of 0.6) when resting orders last 50 seconds.
- **Informed traders barely cost the market maker.** Its fills against them lose about 0.9 ticks
  per lot. But by pulling prices back to value they make its fills against noise traders worth
  one to two ticks more, and the noise traders pay for both.
- **What's missing.** Volatility clusters for an hour, not the weeks of real markets, and nothing
  is calibrated to real order books yet.

![Mid move after the aggressive orders of noise traders, trend followers and informed traders](docs/images/impact.png)

The analysis is a separate Python package that drives the `crowdbook` command:

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-facts
uv run --project analysis crowdbook-experiments
uv run --project analysis crowdbook-large-orders
```

## Trade in it yourself

`crowdbook play` runs a scenario in real time on a trading screen in the terminal, with you as
one more trader:

```bash
./build/release/apps/crowdbook play examples/scenarios/playable.toml --record session.toml
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
./build/release/apps/crowdbook replay session.toml --log session.csv
```

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

## Building

Each workflow preset configures, builds and runs the test suite, with output in `build/<preset>/`.

| Preset    | Build                                                      |
|-----------|------------------------------------------------------------|
| `dev`     | Debug                                                      |
| `release` | Release, including the benchmarks                          |
| `asan`    | Debug with AddressSanitizer and UndefinedBehaviorSanitizer |

The core library has no dependencies. Scenario files use
[toml++](https://github.com/marzer/tomlplusplus) and the tests use GoogleTest; CMake fetches both
at pinned versions, and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has toml++'s license,
since it is compiled into the `crowdbook` command. The analysis in `analysis/` is a
[uv](https://docs.astral.sh/uv/) project using polars, NumPy and matplotlib; nothing in the C++
build depends on it.

## Performance

Measured on an Apple M5 with a Release build:

- The matching engine handles about 30 million operations per second (roughly 33 ns each) on a
  mixed stream of passive orders, cancels, crossing orders and market orders.
- A simulated day of the 1,041-agent mixed market in
  [large_market.toml](examples/scenarios/large_market.toml), 17 million trades, takes 41 seconds
  on one core: about 2,100 times faster than real time.
- Whole markets of zero-intelligence traders, each acting about four times a second, simulate
  this many seconds per second of wall-clock time:

  | Traders | Every update pushed to every trader | Traders read the market on demand |
  |---:|---:|---:|
  | 100 | 1,076 | 9,641 |
  | 1,000 | 10.1 | 608 |
  | 10,000 | — | 44 |

  Pushing every trade and quote change to every trader costs N² as the crowd grows, so agents
  that only look at the market when they act read it on demand instead. Market makers, which
  react to every trade, still get the full stream.

To reproduce:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench
```

## License

[MIT](LICENSE). Third-party code compiled into the binary is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
