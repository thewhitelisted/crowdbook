# Quickstart

From nothing to a market running, a market you trade in, a bot, a learning agent, and a fitted
market, each in a few commands. [tutorial.md](tutorial.md) then takes a recording all the way to a
tested strategy.

## Install

crowdbook runs on Linux and macOS. It comes as three Python packages:

| package | holds | licence |
|---|---|---|
| `crowdbook-engine` | the `crowdbook` command, and `crowdbook_learn`, markets stepped from Python | [PolyForm Shield 1.0.0](../LICENSE) |
| `crowdbook-analysis` | fitting and checking markets: `crowdbook-assess`, `crowdbook-calibrate`, `crowdbook-validate`, `crowdbook-sweep`, `crowdbook-draws`, `crowdbook-generate`, and the experiments behind [results.md](results.md) | PolyForm Shield 1.0.0 |
| `crowdbook` | the client for trading in a served market from Python, standard library only | [MIT](../clients/python/LICENSE) |

The examples and the tutorial's files are in the repository, so start from a clone and a virtual
environment with Python 3.12 or later:

```bash
git clone https://github.com/thewhitelisted/crowdbook && cd crowdbook
python3 -m venv .venv && . .venv/bin/activate
```

**From a release's wheels**, with no compiler: each release on GitHub carries wheels for Linux
(x86-64 and arm64) and macOS (Apple silicon). For release `vX.Y.Z`:

```bash
pip install crowdbook-engine crowdbook-analysis \
    --find-links https://github.com/thewhitelisted/crowdbook/releases/expanded_assets/vX.Y.Z
```

**From source**, with CMake 3.25 or later and a C++23 compiler (GCC 14, or Apple clang from
Xcode 16), which builds the engine in a minute or two:

```bash
pip install . ./analysis              # and ./clients/python for the client
```

Check it:

```bash
crowdbook --version
python -c "import crowdbook_learn"
```

**For working on crowdbook itself**, build with CMake's presets and run the tests; the analysis
commands then use the build, through [uv](https://docs.astral.sh/uv/):

```bash
cmake --workflow --preset release     # builds and tests into build/release
uv run --project analysis crowdbook-assess ...
```

The analysis commands look for the `crowdbook` command in this order: `--crowdbook PATH`, the
`CROWDBOOK` environment variable, `build/release/apps/crowdbook` in a checkout, and `crowdbook` on
the path, where the wheel puts it.

## Run a market

```bash
crowdbook run examples/scenarios/market_maker.toml --log run.csv --json result.json
```

A scenario file says who trades and how; [scenarios.md](scenarios.md) has every key and
`crowdbook agents` every kind of trader. The same seed gives the same run, byte for byte, on any
machine. `--log` writes every order, fill and quote change, `--prices` and `--depth` sample the
book, `--duration` and `--seed` override the file.

## Trade in it yourself

```bash
crowdbook play examples/scenarios/playable.toml --record session.toml
crowdbook report session.toml
```

A price ladder in the terminal, at a speed you choose. Afterwards, the report says who you traded
with, what they knew, and what the market would have done without you. The
[challenges](../examples/challenges) score you at a task: making markets, working a large order,
trading the news.

## Write a bot

`crowdbook serve` puts a market on the network, with seats for people and programs:

```bash
crowdbook serve examples/challenges/market_making.toml --seat bot
```

```bash
pip install ./clients/python
python clients/python/examples/market_maker.py --seat bot
```

The clock starts when every seat is claimed, and the market runs in real time, as the challenge
sets it; ctrl-c ends it early. Any language that can open a socket can trade; [protocol.md](protocol.md) is the protocol, JSON
lines, and [clients/python/README.md](../clients/python/README.md) the Python client.

## Step markets from Python

```python
import crowdbook_learn

pool = crowdbook_learn.Pool("examples/learning/market.toml", markets=64)
obs, info = pool.reset(seeds=range(64))
obs, rewards, terminated, truncated, info = pool.step(actions)  # 64 rows of (action, size)
```

Many markets at once, in C++ on threads of their own, each through one seat for a learning agent,
and behind Gymnasium's vector API with `crowdbook_learn.VectorEnv` (`pip install gymnasium`). A
strategy that names its own prices trades through `Pool.step_orders`, and
`crowdbook_learn.evaluate` reports what it made, its fills and its adverse selection across many
markets. [python/README.md](../python/README.md) has the details.

## Fit a recorded market

```bash
crowdbook-assess SPEC --real RECORDING --output assessed
```

A recording in crowdbook's tape format, a scenario with blanks for the settings to fit, and their
ranges: the fit, whether it reaches the recording within chance, whether the fitted market pays
any generic strategy, and a verdict on whether it can be trusted for testing strategies.
[tutorial.md](tutorial.md) walks through it on a recording whose answer you can check, and
[validation.md](validation.md) is the reference.

## When something goes wrong

- **`crowdbook not found`** from an analysis command: install `crowdbook-engine` in the same
  environment, or pass `--crowdbook PATH`.
- **A scenario error** names the file, the line and the key: `examples/` has a working file for
  most features to compare with.
- **CMake cannot download toml++ or nanobind** when building from source: the build fetches them at
  pinned versions from GitHub and PyPI, so it needs the network the first time.
- Anything else: an issue on [GitHub](https://github.com/thewhitelisted/crowdbook/issues).
