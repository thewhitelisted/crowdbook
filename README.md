# crowdbook

An agent-based limit order book simulator for market microstructure research, written in C++23.

crowdbook models a market as a crowd of individual traders — market makers, informed traders,
momentum traders, noise traders — each sending orders to a simulated exchange. Spreads, depth,
price impact and volatility are not assumed; they emerge from how the agents interact. Writing
your own agent means writing one C++ class and adding it to a scenario.

> **Status:** early development. Milestone M0 (build, tests, CI) is in place; the order book is
> next. See [docs/design.md](docs/design.md) for the architecture and roadmap.

## Building

Requires CMake 3.25+, Ninja, and a C++23 compiler (CI covers Apple clang and GCC 14).

```bash
cmake --workflow --preset dev
```

Each workflow preset configures, builds and runs the test suite, with output in `build/<preset>/`.

| Preset    | Build                                                      |
|-----------|------------------------------------------------------------|
| `dev`     | Debug                                                      |
| `release` | Release                                                    |
| `asan`    | Debug with AddressSanitizer and UndefinedBehaviorSanitizer |

## License

[MIT](LICENSE)
