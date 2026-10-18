# Tutorial: from a recording to a tested strategy

In about fifteen minutes, five of them waiting on your computer, you take a recorded market, fit
crowdbook's simulator to it, ask whether the fitted market can be trusted for testing
strategies, and test a strategy in it. It is the path any recording takes, whatever market it
comes from:

1. **A recording**, in crowdbook's format for markets, a *tape*.
2. **A fit**: the simulator's settings that make its markets look like the recording, statistic
   by statistic, with uncertainties.
3. **A verdict**: does the fit reach the recording within chance, and does the fitted market pay
   any generic strategy? A fitted market that pays a naive strategy the real one does not would
   make every strategy tested in it look better than it is.
4. **A strategy tested** in the fitted market and across the fit's uncertainty: what it makes,
   its fills, what they were worth a while later, and how much it can trade.

Here the recording is itself simulated, from settings kept hidden until the end, so that you can
check every answer the tools give you. The commands are the same for a real one.

Every number below came from a run on four cores; yours will differ a little with your
machine's, never with the seeds, which fix every market.

## 0. Install

You need Python 3.12 or later, and the repository for the tutorial's files:

```bash
git clone https://github.com/thewhitelisted/crowdbook && cd crowdbook
python3 -m venv .venv && . .venv/bin/activate
pip install . ./analysis       # builds the engine: CMake 3.25+ and a C++23 compiler
```

That installs two packages: `crowdbook-engine`, the `crowdbook` command and the learning pool
`crowdbook_learn`, and `crowdbook-analysis`, the commands that fit and check markets. A release's
ready-built wheels for Linux and macOS skip the compiler; [quickstart.md](quickstart.md#install)
says how to install them. Check it worked:

```bash
crowdbook --version
```

## 1. The recording

```bash
crowdbook-generate examples/tutorial/spec.toml --markets 100 --seed 2026 \
    --fixed maker_intensity=0.8,noise_market_rate=0.3,follower_lag=10 --out recording
```

That is 100 five-minute markets, in about three seconds. Pretend someone handed them to you: in
`recording/shard-00000` is a tape, the format every recording is converted to
([validation.md](validation.md#tapes-one-format-for-every-market)):

| table | what it holds |
|---|---|
| `markets` | one row a market: its length, and labels |
| `book` | the best levels of the book, sampled every 100 ms |
| `trades` | every print: time, price, size, and which side took liquidity |
| `value` | a reference price; here, the asset's true value |

A real recording, of any market where you can see the book and the trades, becomes a tape once,
with a short script of your own; nothing else in crowdbook knows where a tape came from.

`recording/markets.csv` holds the answer: the settings it was made with. Don't look yet.

## 2. The model to fit

[examples/tutorial/market.toml](../examples/tutorial/market.toml) is a scenario, crowdbook's
description of a market, with three blanks:

- **makers**, three market makers quoting around the asset's true value as they see it, a tenth of a
  second late; `${maker_intensity}` sets how wide they quote, about 1/k ticks either side;
- **noise traders**, twenty who send orders at random, `${noise_market_rate}` market orders a second
  each;
- **followers**, twenty who trade toward the true value, but see it `${follower_lag}` late.

Every trader pays a fee of a tick a lot, maker or taker. [spec.toml](../examples/tutorial/spec.toml)
gives each blank a range to search and a starting point, deliberately off: makers twice as wide
as the truth, noise traders a third as busy and followers too quick.

[scenarios.md](scenarios.md) describes every key of a scenario, and `crowdbook agents` lists the
kinds of trader.

## 3. Fit and verdict

```bash
crowdbook-assess examples/tutorial/spec.toml --real recording/shard-00000 --output assessed
```

About two and a half minutes, 9,160 simulated markets: 5,060 for the fit and the rest for the
check. It does three things.

**It fits.** The method of simulated moments: about 48 statistics of the book, the trades and the
prices, measured alike on the recording and on simulated markets, and a search (CMA-ES) for the
settings whose markets are closest, each statistic counted in units of its own sampling error.
`assessed/report.md` has every statistic, recorded against the start and the fit:

| setting | start | fitted | 68% interval |
|---|---:|---:|---:|
| maker_intensity | 0.4 | 0.826 | 0.816 to 0.836 |
| noise_market_rate | 0.1 | 0.291 | 0.283 to 0.298 |
| follower_lag | 3 s | 9.6 s | 8.9 to 10.4 s |

**It judges the fit on seeds the search never saw.** Its distance there is 41.6, where chance
alone stays under 65.2 95% of the time: the fitted market is the recording's, as far as these
statistics can tell. When a fit does not reach a recording within chance, the model is missing
something, and the report's statistics furthest from the recording say what.

**It looks for false edges.** `crowdbook-sweep` runs 25 generic strategies alone in the fitted
market (makers at half widths of 0.5 to 8 ticks, trend followers, reversal traders, order-flow
followers) and judges each family's best on 400 fresh markets:

| each family's best, on fresh markets | made a market (tick-lots) |
|---|---:|
| maker, 3 ticks either side | +0.1 [−14.8, +15.1] |
| trend, 2 s against 20 s | −15.3 [−47.2, +16.5] |
| reversal, 60 s against 600 s | −72.4 [−99.1, −45.6] |
| flow, 120 s at 80% | +9.2 [−17.6, +36.0] |

None makes money for sure, after fees. So `assessed/verdict.md` says:

> **Usable for testing strategies**: the fit reaches the recording within chance and no generic
> strategy makes money in it.

and the command exits with 0, 1 when it is not usable, so that a script can act on it. When it
says no, it says why: the statistics furthest from the recording, and any strategy the fitted
market pays. It also shows what a lot quoted 1, 3 and 8 ticks from the mid would have earned in
the recording and in the fit, measured alike: where the recording pays such a quote as well as the
fit does, a maker's flag is the market's own edge; where it does not, the flag is the fit's, and
nothing tested in that fit can be trusted until the fit is corrected.

## 4. A strategy, tested

[examples/tutorial/strategy.py](../examples/tutorial/strategy.py) is a strategy of a dozen lines: a
maker that joins the best bid and the best offer with two lots, and stops adding to its position
past ten lots. It trades through a seat in the market, as an outside trader, with a millisecond's
latency, and it pays the fees.

A fit is a point and an uncertainty, so test the strategy across both: draw 20 markets from the
fit's intervals, doubled since intervals from a search without refits are about half as wide as
they should be ([validation.md](validation.md#recovering-known-settings)):

```bash
crowdbook-draws examples/tutorial/spec.toml --report assessed/report.json --count 20 --out draws
python examples/tutorial/strategy.py assessed/scenario.toml draws
```

Each row runs the strategy on 200 markets, about twenty seconds:

| market | made a market (tick-lots) | 95% interval | lots | per lot | markout 10 s |
|---|---:|---|---:|---:|---:|
| the fit | −879.7 | [−907.6, −851.8] | 1,512 | −0.582 | +0.382 |
| 20 draws from the fit | −884.7 | [−912.8, −856.6] | 1,505 | −0.588 | +0.369 |

It loses about 0.58 ticks a lot. Its fills are worth +0.38 ticks ten seconds on, before fees: it
earns part of the spread and is not picked off, but the fee of a tick a lot takes more than all
of it. The markout is what tells you whether a change could save it: here the fee, not adverse
selection, is the problem, so quoting wider might, and trading faster would not.
`crowdbook_learn.evaluate` reports more than the table shows: the lots ahead of each quote in the
queue, the share of quoted lots that filled, how long a fill waited, markouts at 1, 10 and 60
seconds, and with `scales=(1, 2, 4)` the same at larger sizes, which says how much the market can
take ([python/README.md](../python/README.md)).

## 5. The answer

Now look at `recording/markets.csv`: the recording was made with makers at 0.8, noise traders at
0.3 and followers 10 seconds late. The fit found 0.826, 0.291 and 9.6 s, each within 4%. Its
intervals cover the truth for the followers only; with `--refits 4` the search is repeated from
the fit on fresh seeds and the intervals count its own noise, and cover it more often (4 of 5 in [validation.md](validation.md#recovering-known-settings)'s
check).

And the strategy in the true market:

```bash
python examples/tutorial/strategy.py --truth maker_intensity=0.8,noise_market_rate=0.3,follower_lag=10
```

| market | made a market (tick-lots) | 95% interval | lots | per lot | markout 10 s |
|---|---:|---|---:|---:|---:|
| the truth | −854.8 | [−882.7, −827.0] | 1,538 | −0.556 | +0.403 |

The fit predicted −880 a market where the truth gives −855, their intervals overlapping, and
the same fills, the same markouts and the same verdict: don't run it. With a real recording
there is no truth to look up. The test that takes its place is paper trading: run the strategy
on the live market without sending orders, and set its fills beside the fitted market's.

## Where next

- **Your own recording:** convert it to a tape ([validation.md](validation.md#tapes-one-format-for-every-market)),
  copy the tutorial's spec and template, and fit the settings you care about. Settings you
  cannot fit, because the book's statistics barely move with them, the recovery check finds:
  `crowdbook-calibrate SPEC --truth name=value,...` fits markets made at settings you know and
  says whether each interval covers them.
- **Markets that differ from one another**, such as games, each with its own kickoff, opening
  price and scores: fit each recorded market against twins of its own
  ([validation.md](validation.md#twins)).
- **Your own strategy in Python:** any function from the markets' observations to orders,
  through `crowdbook_learn.evaluate`; or a learning agent, through `crowdbook_learn.Pool` and
  Gymnasium's vector API ([python/README.md](../python/README.md)).
- **Your own trader in the crowd:** a C++ class against a small API
  ([examples/custom_agent.cpp](../examples/custom_agent.cpp), [library.md](library.md)).
- **How it works, and how it is tested:** [design.md](design.md).
