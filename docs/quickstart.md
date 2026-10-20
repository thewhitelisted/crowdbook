# Quickstart

From nothing to a market running, a market you trade in, a bot, a learning agent, and a fitted
market, each in a few commands. [tutorial.md](tutorial.md) then takes a recording all the way to a
tested strategy.

## Install

crowdbook runs on Linux and macOS. It comes as three Python packages:

| package | holds | licence |
|---|---|---|
| `crowdbook-engine` | the `crowdbook` command, and `crowdbook_learn`, markets stepped from Python | [PolyForm Shield 1.0.0](../LICENSE) |
| `crowdbook-analysis` | fitting and checking markets: `crowdbook-assess`, `crowdbook-calibrate`, `crowdbook-validate`, `crowdbook-sweep`, `crowdbook-draws`, `crowdbook-generate`, `crowdbook-tape`; the experiments behind [results.md](results.md) run from a checkout, with `uv run --project analysis` | PolyForm Shield 1.0.0 |
| `crowdbook` | the client for trading in a served market from Python, standard library only | [MIT](../clients/python/LICENSE) |

The examples and the tutorial's files are in the repository, so start from a clone and a virtual
environment with Python 3.12 or later:

```bash
git clone --branch v0.14.1 https://github.com/thewhitelisted/crowdbook && cd crowdbook
python3 -m venv .venv && . .venv/bin/activate
```

**From PyPI's wheels**, with no compiler: each release carries wheels for Linux
(x86-64 and arm64, glibc 2.28 or later) and macOS 14 or later on Apple silicon. For the latest
release:

```bash
pip install crowdbook-engine==0.14.1 crowdbook-analysis==0.14.1
```

On any other platform that command builds the engine from its source package instead, which
needs the compiler below.

**From source**, for other platforms or a checkout of `main`, with CMake 3.25 or later and a
C++23 compiler (GCC 14, or Apple clang from Xcode 16), which builds the engine in a minute or
two:

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
pip install crowdbook==0.14.1
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
any strategy of the check, generic or learned, and a verdict on whether it can be trusted for testing strategies.
[tutorial.md](tutorial.md) walks through it on a recording whose answer you can check, and
[validation.md](validation.md) is the reference.

## Your own recording

A recording in plain CSV, as order-book data usually comes, converts to a tape:

```bash
crowdbook-tape --quotes quotes.csv --trades trades.csv --market 3600 --tick 0.01 \
    --returns log --out recording
```

- `quotes.csv`: `time`, `bid_price`, `bid_size`, `ask_price`, `ask_size`, and deeper levels as
  `bid_price_2`, `bid_size_2` and so on; each row the book as it stood from then.
- `trades.csv`: `time`, `price`, `size`, and `side`, the side that took liquidity, `buy` or
  `sell`, or empty to have it inferred from the book.
- `--value value.csv`, optional: `time`, `value`, a reference price.
- Times are seconds, or another unit with `--time-unit ms`, or dates and times as text.
- `--market 3600` cuts a long recording into markets an hour long, each opening with the book and
  the value as they stood. A recording is judged over its markets, its intervals from resampling
  them, so a day of one market is better read as many shorter ones. An hour with no quotes or
  trades of its own, such as one the venue was closed, is left out, and the last market ends at
  the recording's last quote or trade.
- `--returns log` for prices like a stock's or a coin's; the default, `difference`, for bounded
  prices such as a probability's.
- `--same-order 0.002` for a venue that stamps each fill of one order on its own: prints within
  that many seconds of the one before count as one order.

[examples/recording](../examples/recording) is such a recording to try it on: twelve of the
tutorial's five-minute markets laid end to end into an hour, each one's five best levels, trades
and a reference price once a second, gzipped CSV as data feeds often come, with prices in dollars
at a cent a tick. `--market 300` cuts it back into those markets; each starts afresh at $100. It
is in the clone from the install steps:

```bash
crowdbook-tape --quotes examples/recording/quotes.csv.gz --trades examples/recording/trades.csv.gz \
    --value examples/recording/value.csv.gz --market 300 --tick 0.01 --returns log --out recording
crowdbook-assess examples/tutorial/spec.toml --real recording --output assessed
```

It was made by the tutorial's market at the tutorial's settings, 0.8, 0.3 and 10 s, and the fit
comes close (maker intensity 0.84, noise traders 0.33, followers 9.0 s late) and judges it usable,
in about five minutes. Without `--value` the same fit is judged not usable, far from the
recording: the tutorial's followers trade toward the value, and only the statistics of how the
price follows it tell how late they are. A model whose traders follow a value needs a recording
with one, such as an index or a fair price; for a recording without, fit a model whose traders
need none.

The tape goes wherever `--real` does: `crowdbook-validate`, `crowdbook-calibrate` and
`crowdbook-assess`. [validation.md](validation.md#tapes-one-format-for-every-market) describes the
format, for recordings that need a converter of their own.

## When something goes wrong

- **`crowdbook not found`** from an analysis command: install `crowdbook-engine` in the same
  environment, or pass `--crowdbook PATH`.
- **A scenario error** names the file, the line and the key: `examples/` has a working file for
  most features to compare with.
- **CMake cannot download toml++ or nanobind** when building from source: the build fetches them at
  pinned versions from GitHub and PyPI, so it needs the network the first time.
- Anything else: an issue on [GitHub](https://github.com/thewhitelisted/crowdbook/issues).
