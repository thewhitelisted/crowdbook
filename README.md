# crowdbook

An agent-based limit order book simulator for market microstructure research, written in C++23.

crowdbook models a market as a crowd of individual traders — market makers, informed traders,
trend followers, noise traders — each sending orders to a simulated exchange over its own network
link. Spreads, depth, price impact and volatility are not assumed; they emerge from how the agents
interact. Writing your own agent means writing one C++ class and naming it in a scenario file.

> **Status:** the order book, the exchange, the simulation kernel, four built-in agents, scenario
> files and the `crowdbook` command are done; analysis tooling and the first experiments are next.
> See [docs/design.md](docs/design.md) for the architecture, the testing approach and the roadmap.

## Quick start

Requires CMake 3.25+, Ninja and a C++23 compiler (CI covers Apple clang and GCC 14).

```bash
cmake --workflow --preset dev
```

```bash
./build/dev/apps/crowdbook run examples/scenarios/market_maker.toml --log run.csv
```

```
examples/scenarios/market_maker.toml (seed 7): simulated 60s in 0.07s
1726 trades, 6247 lots traded, last price 10010

group                 agents   position           cash          pnl
maker                      1        -46         463356        +2896
noise                     20         46        -463356        -2896
```

A market maker with a fast connection earns the spread from twenty zero-intelligence traders.
`run.csv` holds every order, cancel, fill, trade and top-of-book change. The same seed always
reproduces the same run, byte for byte. [examples/scenarios](examples/scenarios) has four
scenarios, including one with trend followers and one where informed traders know the asset's
true value; [docs/scenarios.md](docs/scenarios.md) describes the file format and every agent
parameter.

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

The core library has no dependencies. Scenario files use [toml++](https://github.com/marzer/tomlplusplus)
and the tests use GoogleTest; CMake fetches both at pinned versions.

## Performance

Measured on an Apple M5 with a Release build:

- The matching engine handles about 15 million operations per second (roughly 65 ns each) on a
  mixed stream of passive orders, cancels, crossing orders and market orders.
- Whole markets of zero-intelligence traders, each acting about four times a second, simulate
  this many seconds per second of wall-clock time:

  | Traders | Every update pushed to every trader | Traders read the market on demand |
  |---:|---:|---:|
  | 100 | 1,295 | 9,815 |
  | 1,000 | 8.5 | 734 |
  | 10,000 | — | 52 |

  Pushing every trade and quote change to every trader costs N² as the crowd grows, so agents
  that only look at the market when they act read it on demand instead. Market makers, which
  react to every trade, still get the full stream.

To reproduce:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench
```

## License

[MIT](LICENSE)
