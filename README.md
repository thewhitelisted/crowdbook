# crowdbook

An agent-based limit order book simulator for market microstructure research, written in C++23.

crowdbook models a market as a crowd of individual traders — market makers, informed traders,
momentum traders, noise traders — each sending orders to a simulated exchange. Spreads, depth,
price impact and volatility are not assumed; they emerge from how the agents interact. Writing
your own agent means writing one C++ class and adding it to a scenario.

> **Status:** early development. The order book, the exchange (validation, position limits,
> settlement, market data) and the simulation kernel (per-agent latency, the agent API,
> reproducible runs, CSV event log) are done; built-in agents and scenario files are next. See
> [docs/design.md](docs/design.md) for the architecture, the testing approach and the roadmap.

## Building

Requires CMake 3.25+, Ninja, and a C++23 compiler (CI covers Apple clang and GCC 14).

```bash
cmake --workflow --preset dev
```

Each workflow preset configures, builds and runs the test suite, with output in `build/<preset>/`.

| Preset    | Build                                                      |
|-----------|------------------------------------------------------------|
| `dev`     | Debug                                                      |
| `release` | Release, including the benchmarks                          |
| `asan`    | Debug with AddressSanitizer and UndefinedBehaviorSanitizer |

## Performance

The matching engine handles about 15 million operations per second (roughly 65 ns each) on a
mixed stream of passive orders, cancels, crossing orders and market orders, measured on an Apple
M5 with a Release build. To reproduce:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench
```

## License

[MIT](LICENSE)
