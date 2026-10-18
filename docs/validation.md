# Validation, calibration and datasets

A synthetic market is worth using only as far as it is shown to be the market it stands for. This
is how crowdbook shows it: one format for recorded and simulated markets, a report that sets their
statistics side by side with intervals and distances, a calibration that fits any scenario to a
recording, and a generator of many labelled markets for learners and analysts. All of it is in the
Python package in [`analysis/`](../analysis), and works with the Release build:

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-validate --real RECORDED --scenario my_market.toml
uv run --project analysis crowdbook-calibrate my_spec.toml --real RECORDED
uv run --project analysis crowdbook-generate my_spec.toml --markets 10000 --out dataset
```

## Tapes: one format for every market

A tape is a directory of tables, CSV or Parquet, holding one market or many
([tape.py](../analysis/src/crowdbook_analysis/tape.py)). Times are seconds from each market's
start; prices are in any one unit, the same throughout.

| table | columns | |
|---|---|---|
| `markets` | `market`, `duration`, optional `outcome`, any labels | required: one row per market; `outcome` is what one unit paid at settlement; other columns are labels, such as a simulated market's hidden settings |
| `book` | `market`, `time`, `bid_price_1`, `bid_size_1`, `ask_price_1`, `ask_size_1`, ... to any depth | required: snapshots, each holding until the next, at any times; a missing level is empty |
| `trades` | `market`, `time`, `price`, `size`, `side` | required: one row per print; `side` is the side that took liquidity, `buy` or `sell`, inferred from the last quote when empty |
| `value` | `market`, `time`, `value` | optional: a reference value to set the price against |

An optional `tape.toml` gives `tick`, the price increment (1 if absent), `returns`,
`"difference"` (the default, for bounded prices such as probabilities) or `"log"`, and for a binary
contract `payout`, what a winning unit pays. A recording is converted to a tape once, by whoever
holds it; `tape.simulate` runs any scenario under many seeds into one, its book sampled every
100 ms.

## The validation report

`crowdbook-validate` ([validate.py](../analysis/src/crowdbook_analysis/validate.py)) measures the
same statistics on a real tape and on one or more synthetic ones:

| family | statistics |
|---|---|
| the book at the touch | the spread, time-weighted, its median and the share of time at one tick; size at the best quotes, and within five levels; how often a side is empty |
| trades | aggressive orders a minute (the prints one order makes at one time count once), prints per order, order sizes (median, the largest tenth against the median, the tail's Hill exponent), how bursty they come, and the share in the first and last tenth of the market |
| returns | the mid's changes over 1, 10 and 60 s: standard deviation, excess kurtosis, the tail exponent, variance ratios, successive changes' autocorrelation |
| volatility clustering | the autocorrelation of the size of 1 s changes, 1 and 10 s apart |
| order-flow memory | the autocorrelation of order signs 1, 10 and 50 orders apart; the bounce in trade prices and Roll's spread from it |
| what resting orders earn | per unit filled, size-weighted: from the mid just before (the effective half spread), marked to the mid 1, 10 and 60 s later, and at settlement |
| how fast the mid follows the value | of a 1 s move of the value, the share the mid has made 0, 0.5, 1, 2 and 5 s after it; the delay at which the mid is closest to the value as it was; the mean distance |
| calibration | for a binary contract, the Brier score of the mid, and how often prices of 10 to 20% and 80 to 90% of the payout paid out |

Each is pooled over markets, its interval the 5th to 95th percentile over 200 resamples of
markets with replacement, since markets, not moments, are independent. A synthetic statistic's
distance from the real one is their difference over the two resampling spreads together, so that
about 95% of honest statistics fall within 2; spreads, sizes and rates are compared as logarithms.
The report ends with how many statistics are within 2 and their sum of squares, then the
Kolmogorov–Smirnov distance between the pooled distributions of the spread, the size at the touch,
order sizes and 1 s changes, beside the distance between two random halves of the real markets,
which is what chance alone gives at that many markets.

`--window START:END` measures only that part of each market, in seconds from its start, with the
book as it stood at `START`: before kickoff and during play, say, or the part a recording covers.

How far the mid follows the value is measured on the book's own snapshots, so the shortest lags
need snapshots at least that often: a tape sampled every second shows the same share at 0 and
0.5 s.

### What it says of the engine's own markets

The same scenario under other seeds is the market itself, so it should pass; another scenario
should not. With 40 markets each of the fitted fifteen-minute question
([up_or_down.toml](../examples/scenarios/up_or_down.toml)) as the target, and 50 resamples:

| | within 2 | sum of squares | KS distances (chance 0.022, 0.041, 0.010, 0.017) |
|---|---:|---:|---|
| up_or_down, other seeds | 40 of 46 | 76 | 0.015, 0.014, 0.005, 0.006 |
| prediction.toml | 9 of 46 | 9,783 | 0.061, 0.256, 0.355, 0.055 |

40 of 46 within 2 is fewer than the 44 that pure chance would give: 40 markets are few for a
bootstrap, whose spread they understate, and some statistics move together. Read a distance of 2
to 3 as borderline, not as a failure.

## Calibration

`crowdbook-calibrate` ([calibrate.py](../analysis/src/crowdbook_analysis/calibrate.py)) fits any
scenario to a tape. A spec names a template, a scenario with `${name}` wherever a setting goes, and
each setting's range ([book_spec.toml](../examples/calibration/book_spec.toml) and
[book.toml](../examples/calibration/book.toml) are one):

```toml
template = "book.toml"
statistics = ["spread_mean", "orders_per_minute"]   # optional: default, every one both have
seeds = 40            # markets each candidate is judged on, the same for every candidate
generations = 80
held_out = 200        # markets the fit is judged on afterwards, on other seeds
window = [0, 1800]    # optional: compare only this part of each market, in seconds

[parameters.follower_lag]
low = 0.5
high = 30.0
start = 5.0           # optional: default, the middle of the range on its scale
kind = "duration"     # written as "250ms"; "number" otherwise
# log = true          moves in proportion: the default where low > 0
# whole = true        a count, such as of traders
```

The method is that of the fifteen-minute fit in [results.md](results.md#a-market-fitted-to-them),
made general: the method of simulated moments, tolerances from resampling the target and the
starting market, CMA-ES over every setting at once on common seeds, a verdict on held-out seeds,
and 68% intervals from the sandwich formula. It writes the fitted scenario, the report of the start
and the fit against the target, and the settings with their intervals, as Markdown and JSON.

Two checks guard the fit:

- **Does it reach the target?** At the true settings every compared statistic is off by chance
  alone, so the held-out distance should be below the 95th percentile of chi-squared with a degree
  of freedom a statistic. The report says whether it is. When it is not, the model or the search
  falls short and the intervals, which assume the fit is at the truth, are too narrow to trust.
- **Can it find what it should?** `--truth name=value,...` fits markets the engine made itself, at
  settings it knows, from the spec's starting point, and reports whether each interval covers
  the truth. A fit that cannot find a known answer should not be trusted with an unknown one.

### Recovering known settings

On [book.toml](../examples/calibration/book.toml), five settings, the truth at
maker_intensity 0.8, noise_market_rate 0.3, size_tail 1.2, 20 followers and a follower lag of
10 s, 400 target markets, starting from 0.4, 0.1, 1.5, 10 and 5 s:

The result is being measured and goes here when it is.

## Datasets

`crowdbook-generate` ([generate.py](../analysis/src/crowdbook_analysis/generate.py)) makes many
markets from a spec, each with its settings drawn afresh from the ranges, uniformly on their
scales, and its own seed. It writes shards of 500 markets, each a tape in Parquet, and
`markets.csv` with every market's labels: its drawn settings, its seed, its outcome and, for each
group of traders, what it made after fees (`pnl_<group>`, at settlement or the true value) and the
lots it traded (`traded_<group>`). `dataset.toml` records the template, the ranges and the seeds,
so the dataset can be made again exactly; market *i* ran under seed `first_seed + i`.
`--fixed name=value,...` holds settings. On four cores it makes about 40 five-minute markets a
second with their books sampled every 100 ms.

## Games

A prediction market can be about a game on a clock ([`[prediction.game]`](scenarios.md#predictiongame-optional)):
the home side's chance drifts before kickoff with news, jumps when either side scores, ticks toward
a draw as time runs out and settles when the game ends.
[game.toml](../examples/scenarios/game.toml) puts makers who quote off the probability, sharp
traders who see it a second or two late, and recreational bettors who see it late, lean to the home
side and rate long shots too high (`tilt`), in one such market. Over 200 of its markets the makers
made 0.88 cents a lot traded, the bettors lost 0.92 and the sharps nothing clear; it is an example
to fit, not a fit.
