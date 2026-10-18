# Tutorial: from a recording to a tested strategy

In about fifteen minutes, six of them waiting on your computer, you take a recorded market, fit
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

Every trader pays a fee of two ticks a lot, maker or taker (why two, below). [spec.toml](../examples/tutorial/spec.toml)
gives each blank a range to search and a starting point, deliberately off: makers twice as wide
as the truth, noise traders a third as busy and followers too quick.

[scenarios.md](scenarios.md) describes every key of a scenario, and `crowdbook agents` lists the
kinds of trader.

## 3. Fit and verdict

```bash
crowdbook-assess examples/tutorial/spec.toml --real recording/shard-00000 --output assessed
```

About four and a half minutes on four cores: 5,060 simulated markets for the fit, and the rest
for the check. It does three things.

**It fits.** The method of simulated moments: about 47 statistics of the book, the trades and the
prices, measured alike on the recording and on simulated markets, and a search (CMA-ES) for the
settings whose markets are closest, each statistic counted in units of its own sampling error.
`assessed/report.md` has every statistic, recorded against the start and the fit:

| setting | start | fitted | 68% interval |
|---|---:|---:|---:|
| maker_intensity | 0.4 | 0.822 | 0.813 to 0.830 |
| noise_market_rate | 0.1 | 0.303 | 0.295 to 0.311 |
| follower_lag | 3 s | 9.5 s | 8.8 to 10.3 s |

**It judges the fit on seeds the search never saw.** Its distance there is 20.9, where chance
alone stays under 64.0 95% of the time: the fitted market is the recording's, as far as these
statistics can tell. When a fit does not reach a recording within chance, the model is missing
something, and the report's statistics furthest from the recording say what.

**It looks for false edges.** `crowdbook-sweep` runs 25 generic strategies alone in the fitted
market (makers at half widths of 0.5 to 8 ticks, trend followers, reversal traders, order-flow
followers), and trains a learner there too, a strategy of its own that weighs the recent order
flow and trend, to find what the generic ones miss. Each family's best, and the learner, are
judged on 400 fresh markets:

| each family's best, on fresh markets | made a market (tick-lots) |
|---|---:|
| maker, 8 ticks either side | −0.8 [−1.8, +0.3] |
| trend, 60 s against 600 s | −24.8 [−56.8, +7.2] |
| reversal, 60 s against 600 s | −87.8 [−114.2, −61.4] |
| flow, 30 s at 80% | −27.0 [−55.7, +1.7] |
| learner, 20 generations on 16 markets | −7.1 [−42.8, +28.7] |

None makes money for sure, after fees. So `assessed/verdict.md` says:

> **Usable for testing strategies**: the fit reaches the recording within chance and no strategy
> of the check makes money in it.

Why fees of two ticks: at one tick a lot, the learner makes +87.5 [+58.5, +116.4] a market by
following the last few seconds' order flow, and the verdict is no. That edge is real here: the
recording was made by the very model fitted, so its market pays the same flow-follower. With a
real recording the check cannot tell a market's own edge from one the fit invented; either way,
a strategy tested where a simple one is paid is judged against the wrong bar, and a real fee that
removes the edge, as here, or a closer look at the recording, comes first.

and the command exits with 0, 1 when it is not usable, so that a script can act on it. When it
says no, it says why: the statistics furthest from the recording, and any strategy the fitted
market pays. It also shows what a lot quoted 1, 3 and 8 ticks from the mid would have earned in
the recording and in the fit, measured alike: where the recording pays such a quote as well as the
fit does, a maker's flag is the market's own edge; where it does not, the flag is the fit's, and
nothing tested in that fit can be trusted until the fit is corrected.

## 4. A strategy, tested

[examples/tutorial/strategy.py](../examples/tutorial/strategy.py) is a strategy of a dozen lines: a
maker that joins the best bid and the best offer with two lots, and stops adding to its position
past ten lots: it quotes a side only while a fill there would keep it within ten. It trades through a seat in the market, as an outside trader, with a millisecond's
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
| assessed/scenario.toml | -2,382.2 | [-2,411.9, -2,352.6] | 1,503 | -1.585 | +0.387 |
| 20 scenarios in draws | -2,398.2 | [-2,430.2, -2,366.1] | 1,509 | -1.589 | +0.375 |

It loses about 1.6 ticks a lot. Its fills are worth +0.39 ticks ten seconds on, before fees: it
earns part of the spread and is not picked off, but the fee of two ticks a lot takes more than
all of it. The markout is what tells you whether a change could save it: here the fee, not adverse
selection, is the problem, so quoting wider might, and trading faster would not.
`crowdbook_learn.evaluate` reports more than the table shows: the lots ahead of each quote in the
queue, the share of quoted lots that filled, how long a fill waited, markouts at 1, 10 and 60
seconds, and with `scales=(1, 2, 4)` the same at larger sizes, which says how much the market can
take ([python/README.md](../python/README.md)).

## 5. The answer

Now look at `recording/markets.csv`: the recording was made with makers at 0.8, noise traders at
0.3 and followers 10 seconds late. The fit found 0.822, 0.303 and 9.5 s, each within 5%. Its
intervals cover the truth for the noise traders and the followers, not the makers; with `--refits 4` the search is repeated from
the fit on fresh seeds and the intervals count its own noise, and cover it more often (4 of 5 in [validation.md](validation.md#recovering-known-settings)'s
check).

And the strategy in the true market:

```bash
python examples/tutorial/strategy.py --truth maker_intensity=0.8,noise_market_rate=0.3,follower_lag=10
```

| market | made a market (tick-lots) | 95% interval | lots | per lot | markout 10 s |
|---|---:|---|---:|---:|---:|
| the truth | -2,375.6 | [-2,407.0, -2,344.2] | 1,515 | -1.568 | +0.400 |

The fit predicted −2,382 a market where the truth gives −2,376, their intervals overlapping, and
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
