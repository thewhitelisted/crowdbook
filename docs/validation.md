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

Or all of it in one command, from a recording to a verdict:

```bash
uv run --project analysis crowdbook-assess my_spec.toml --real RECORDED --output assessed
```

[`crowdbook-assess`](../analysis/src/crowdbook_analysis/assess.py) fits the spec to the
recording, runs [the no-false-edge check](#the-no-false-edge-check) on every fitted market (each
recorded market's twin, for a twin fit), and writes `verdict.md` beside the fit's report:
- **Usable or not:** a fit is usable for testing strategies only when it reaches the recording
  within chance and no strategy of the check, the generic probes or
  [a learner trained in it](#a-learner-in-the-check), makes money in it.
- **Why not:** the statistics furthest from the recording, any strategy the fitted market pays,
  and what a standing quote earns in the recording against the fit. Where a maker is flagged and
  the recording's standing quote about as wide earns a fill too, within two tolerances of the
  fit's, the verdict says the flag may be the market's own edge rather than the fit's. The fit
  is still not called usable, since the check cannot tell the two apart for sure.

It exits with 0 for a usable fit and 1 otherwise, so a script or a scheduled job can act on it.

## Tapes: one format for every market

A tape is a directory of tables, CSV or Parquet, holding one market or many
([tape.py](../analysis/src/crowdbook_analysis/tape.py)). Times are seconds from each market's
start; prices are in any one unit, the same throughout, and the report counts them in ticks, so
that markets priced in dollars and in cents compare.

| table | columns | |
|---|---|---|
| `markets` | `market`, `duration`, optional `outcome`, any labels | required: one row per market; `outcome` is what one unit paid at settlement; other columns are labels, such as a simulated market's hidden settings |
| `book` | `market`, `time`, `bid_price_1`, `bid_size_1`, `ask_price_1`, `ask_size_1`, ... to any depth | required: snapshots, each holding until the next, at any times; a missing level is empty |
| `trades` | `market`, `time`, `price`, `size`, `side` | required: one row per print; `side` is the side that took liquidity, `buy` or `sell`, inferred from the last quote when empty |
| `value` | `market`, `time`, `value` | optional: a reference value to set the price against |
| `scores` | `market`, `time`, `home`, `away` | optional, for games: the score after each change, which a calibration's twins can replay |

An optional `tape.toml` gives `tick`, the price increment (1 if absent), `returns`,
`"difference"` (the default, for bounded prices such as probabilities) or `"log"`, and for a binary
contract `payout`, what a winning unit pays. `same_order`, in seconds, is for a venue that stamps
each fill of one order apart: the prints on one side each less than that after the one before are
then one aggressive order (`same_order = 0.001` for books whose fills come microseconds apart).
Without it only prints at one time are one order, as the engine stamps them. A recording is
converted to a tape once, by whoever holds it; `tape.simulate` runs any scenario under many seeds
into one, its book sampled every 100 ms.

## The validation report

`crowdbook-validate` ([validate.py](../analysis/src/crowdbook_analysis/validate.py)) measures the
same statistics on a real tape and on one or more synthetic ones:

| family | statistics |
|---|---|
| the book | the spread, time-weighted, its median and the share of time at one tick; size at the best quotes, within five levels, and within 3 and 10 ticks of the mid; how often a side is empty; how often the best prices change, seen once a grid step alike for a recording and a simulation |
| trades | aggressive orders a minute (the prints one order makes at one time count once, or with the tape's `same_order` each within that of the one before), prints per order, order sizes (median, the largest tenth against the median, the tail's Hill exponent), the gaps between orders (median, and how bursty), the share in the first and last tenth of the market, and lots traded a minute over the size at the touch, how fast the touch is filled |
| returns | the mid's changes over 1, 10 and 60 s: standard deviation, excess kurtosis, the tail exponent, variance ratios, successive changes' autocorrelation |
| volatility clustering | the autocorrelation of the size of 1 s changes, 1 and 10 s apart |
| order-flow memory | the autocorrelation of order signs 1, 10 and 50 orders apart; the bounce in trade prices and Roll's spread from it |
| what resting orders earn | per unit filled, size-weighted: from the mid just before (the effective half spread), marked to the mid 1, 10 and 60 s later, and at settlement |
| what a standing quote would earn | a lot bid and offered 1, 3 and 8 ticks from the mid, filled by any aggressive order that reaches it: fills an hour a side, and per fill marked 60 s later and at settlement. Not in a fit's target unless its spec names it (see the no-false-edge check) |
| how fast the mid follows the value | of a 1 s move of the value, the share the mid has made 0, 0.5, 1, 2 and 5 s after it; the delay at which the mid is closest to the value as it was; the mean distance; how often the best bid is above the value or the best ask below it, and by more than 2 ticks |
| scores, for games | from each score, how long until the touch moves (the median, and the share within 5 s), and until the value moves |
| calibration | for a binary contract, the Brier score of the mid, and how often prices of 10 to 20% and 80 to 90% of the payout paid out |

Each is pooled over markets, its interval the 5th to 95th percentile over 200 resamples of
markets with replacement, since markets, not moments, are independent. A synthetic statistic's
distance from the real one is their difference over the two resampling spreads together, so that
about 95% of honest statistics fall within 2; spreads, sizes and rates are compared as logarithms.
A statistic passes when its distance is within 2, which about 95% of honest ones are. The report
ends with how many pass and their sum of squares, then the
Kolmogorov–Smirnov distance between the pooled distributions of the spread, the size at the touch,
order sizes and 1 s changes, beside the distance between two random halves of the real markets,
which is what chance alone gives at that many markets.

`--plots DIR` also draws the charts: the spread's distribution, the tails of order sizes and of
one-step changes, what resting orders earn by horizon and, where there is a value, how fast the mid
follows it, the real market against up to three synthetic ones. `--json FILE` writes every number.

`--phases LABEL` reports the part of each market before the time a label gives and the part from
it apart, such as before and after `kickoff`, which a simulated game is labelled with, and which a
recording of games can carry. Simulated games also carry their scores, so the score statistics
compare.

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
phase = "from kickoff" # optional: or the part from (or "before") a time a label of each gives
around = { label = "kickoff", before = 3600 }   # optional: trim each recording first (below)
sample_interval = 0.1 # optional: seconds between samples of the simulated book

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
and the fit against the target, and the settings with their intervals, as Markdown and JSON; for
a twin fit, also `twins/`, the fitted twin of every recorded market, one scenario file each.

### Twins

With `condition = true` a spec fits markets that differ from one another, such as games, each
against synthetic twins of its own: the template may use `${duration}`, a market's length in
seconds, any of its labels by name (such as `${kickoff}`), for a binary contract
`${opening_probability}`, where it opened (its first reference value, else its first mid, kept
from 5 to 90%), `${value_path}` and `${outcome}`, its recorded reference value and what it settled
at, in cents, for a twin whose true value is the recorded one
([`path`](scenarios.md#prediction-optional)), and for a game `${script}`, its scores replayed at
the times they came ([`script`](scenarios.md#predictiongame-optional)). `seeds` and `held_out` then count the twins of
each market. Markets are compared with markets alike in what is known about them, and the luck of
each game's result is taken out of the comparison.
[game_twins.toml](../examples/calibration/game_twins.toml) and
[game_twins_spec.toml](../examples/calibration/game_twins_spec.toml) fit a contract on the home
side's win to recorded games this way, or the away side's or a draw, from a `side` label of the
recording (`${side:"home"}`: a placeholder can give what to write where nothing sets it). A draw
cannot open above the chance of a draw in an even game, so draws open at 26% at most. The twins' book is cleared at kickoff (`clear_at`), and their makers stay away in play
(`quiet_from`), or with a `maker_in_play_interval` label requote only that often, as recorded games
show of some kinds of game and not others. [line_twins.toml](../examples/calibration/line_twins.toml) does the same for totals and
spreads, from each market's `side` and `line` labels, the opening price setting how much its twins
score; [prop_twins.toml](../examples/calibration/prop_twins.toml) is a first template for a
player's prop, a total of that player's scoring alone, with thin, wide books and small
recreational takers. Both take each market's `in_play_delay` from its label.

Every twin template has makers who quote only in play, thin and wide, as some sports' books are
quoted in play while others are cleared at kickoff and left: an `in_play_makers` label sets how
many a market has, none without it, and `in_play_intensity`, `in_play_size`, `in_play_lag` and
`in_play_requote` their width, size, lateness and pace, as labels or as settings a spec fits.

Every twin template takes the venue's fees from labels of the recording, `maker_fee` and
`taker_fee` in ticks a lot and `fee_curve` (`"flat"`, or `"probability"` for a fee that scales
with 4p(1 − p)), none where it has no such labels, and the in-play order delay from
`in_play_delay`, so that the twins, and the no-false-edge check on them, charge what the venue
charges.

Where the recording has a reference price, such as a sharper market's mid on the same question,
[reference_twins.toml](../examples/calibration/reference_twins.toml) and
[reference_twins_spec.toml](../examples/calibration/reference_twins_spec.toml) make that the twins'
true value (`${value_path}`) and settle them where the market settled (`${outcome}`). Nothing
about the game's scoring is then fitted or assumed, which the book's statistics could not pin
down anyway (see the game recovery check below), and only the traders are fitted.

`--start SETTINGS` begins the search from an earlier fit's `settings.toml` instead of the spec's
starts, so that a fit can be carried on with more generations or seeds, or after a change to the
template, rather than begun again. Near a fit the distance rises steeply, so a search started
there takes a short first step, 0.02 of each setting's range, where a search from the spec's
starts takes 0.15 (`--step` sets it): carried on from a fit of recorded games, steps of 0.1
scored three to forty times the fit's distance while steps of 0.01 found better points. The
tolerances come partly from the starting point's markets,
so the distances of a fit started elsewhere compare only roughly with the first fit's.

Labels that a template reads as times, such as `kickoff` and `in_play_delay`, are numbers of
seconds, as the templates write `"${kickoff}s"`.

`around` trims each recorded market before anything is fitted, to the span from `before` seconds
ahead of the time a label of its own gives to `after` seconds past it (to its end without
`after`), its time labels moved with it. Its twins are then as short: games recorded from eight
hours before kickoff, trimmed to the last hour and the game, fit in less than half the time, on
the part where the book does something.

`phase` fits one part of each market, cut where a label of its own says, such as a game in play
(`"from kickoff"`) or before it (`"before kickoff"`); a twin is cut where its recorded market is.
A recorded game that sat for eight hours before a two-hour game would otherwise be fitted mostly
on its quiet hours, when in play is where its order delay, its makers' absence and its reaction to
scores show.

The rate of aggressive orders is compared by its count, in the report and in the fit: the signed
root of the Poisson deviance of the recording's count against what the simulated rate expects
over the recording's time, divided by the noise of the simulated count itself (1 plus the
recording's time over the simulation's) and by how much more the simulated markets' counts vary
than a Poisson count would, measured among the twins of each recorded market, which differ only
in their seeds (orders come in bursts; twins of different markets differ by design). Where a
recording saw one order in a day of markets,
resampling its markets cannot tell a quiet fit from one that trades every minute, since the one
market with an order is all it has to resample; the count can, and a fit that makes its market
trade where the recording barely did is held to it. The fit's intervals still weigh the rate by
its resampled tolerance, not its count, so where the count is what pins a setting down, such as
how often the noise traders take, its interval is wider than the count would make it.

A statistic the recording measures in too few of its markets is left out of the target, and the
fit says so: one that more than a tenth of the recording's resamples leave unmeasured, such as the
size of aggressive orders where one recorded market alone had any. Its value would be that one
market's, and its spread across resamples no measure of how far a twin may stray from it.

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

| setting | start | truth | fitted | 68% interval | covers the truth |
|---|---:|---:|---:|---:|---|
| maker_intensity | 0.4 | 0.8 | 0.804 | 0.789 to 0.818 | yes |
| noise_market_rate | 0.1 | 0.3 | 0.291 | 0.285 to 0.298 | no |
| size_tail | 1.5 | 1.2 | 1.20 | 1.19 to 1.21 | yes |
| followers | 10 | 20 | 21 | 20.5 to 21.5 | no |
| follower_lag | 5 s | 10 s | 9.03 s | 8.56 to 9.52 s | no |

80 generations on 40 common seeds, 27,520 markets in all, about 15 minutes on four cores. On 200
held-out seeds the fit is within 2 tolerances on 35 of 40 statistics, a distance of 20.8 against
56.9 that chance stays under, from 81,313 at the start.

- **The settings are found**, each within 1 to 10% of the truth.
- **The intervals are too narrow**: they cover the truth for 2 of 5 settings where 68% would be
  3 or 4, the misses 1.3 to 2 standard errors out. The search picks its best point on the same
  40 seeds it is scored on, and fits their noise as well, which the sandwich formula does not
  count. Read the intervals as about half the real uncertainty, or run the fit with `--refits`.
- **`--refits 4` closes most of the gap.** The same fit, searched again 4 times for 15
  generations from its result on fresh seeds, the spread of where the searches land added to the
  sandwich covariance: the intervals then cover the truth for 4 of 5 settings (maker intensity
  0.779 to 0.822, followers 19.8 to 22.3, follower lag 9.0 to 10.9 s), the miss the noise traders'
  rate at 0.275 to 0.295 against 0.3. The refits measure the simulation's noise again beside the
  search's own, so these intervals lean wide; `crowdbook-draws` does not double them.
- **It takes a long search.** Stopped at 25 generations, the same fit was at a distance of 749 on
  held-out seeds, on a compensating point (30 followers 5 s late instead of 20 at 10 s) with
  intervals just as confident. That is why the report states whether the fit reaches the target
  within chance: a fit that does not cannot be read for its settings.

### Recovering a game's settings

The same check on [game.toml](../examples/calibration/game.toml), ten settings at once: 300 games
at known settings as the target, 100 generations on 30 common seeds, 32,790 games in all, about an
hour on four cores ([game_spec.toml](../examples/calibration/game_spec.toml), before its template held
orders in play).

| setting | start | truth | fitted | 68% interval | covers the truth |
|---|---:|---:|---:|---:|---|
| maker_intensity | 0.8 | 1.2 | 1.20 | 1.18 to 1.22 | yes |
| maker_lag | 1 s | 2 s | 2.08 s | 1.96 to 2.2 s | yes |
| sharp_lag | 1.5 s | 0.8 s | 1.0 s | 0.78 to 1.29 s | yes |
| bettors | 60 | 100 | 101 | 98.9 to 103 | yes |
| tilt | 0.3 | 0.2 | 0.206 | 0.197 to 0.215 | yes |
| bias | 2 | 1 | 0.10 | −0.43 to 0.64 | no |
| probability | 0.5 | 0.6 | 0.537 | 0.483 to 0.59 | no |
| scores | 2.7 | 3 | 3.51 | 3.33 to 3.71 | no |
| late_scoring | 1 | 1.5 | 1.10 | 0.97 to 1.25 | no |
| pregame_drift | 0.15 | 0.25 | 0.072 | 0.057 to 0.090 | no |

The fit reaches the target within chance, a held-out distance of 20.4 against 60.5, 38 of 44
statistics within 2. **Who trades, and how, is found; the game itself is not.** The makers' width
and lateness, the sharp traders' lateness, the bettors' number and their tilt are recovered; how
much is scored and when, the pre-game drift, the opening probability and the bettors' lean to the
home side are not, since the book's statistics, pooled over games, barely move with them. They are
facts of the game, read better where they are recorded: the scores give the scoring rate and its
pace through the game, a reference price gives the opening probability and the drift before
kickoff. So a game is fitted as twins, each opening where its game opened and replaying its
scores, with the scoring settings set from the recorded scores rather than fitted, and the fit
left to the traders.

## The no-false-edge check

A market can match the real one statistic by statistic and still pay a strategy the real one does
not: a book a little wider and thinner than the real one pays a patient maker, a price a little
slower pays whoever follows it. `crowdbook-sweep SCENARIO`
([sweep.py](../analysis/src/crowdbook_analysis/sweep.py)) runs generic strategies, the
[`probe`](scenarios.md#probe) agent at a lot an order, alone in the market, one at a time:

- makers at half widths of 0.5 to 8 ticks around the mid;
- trend followers and reversal traders on moving averages of 2 to 600 seconds;
- flow followers on the imbalance of the last 5 to 120 seconds' taker volume.

Each runs on 200 markets; each family's best is judged on 800 fresh ones, valued at the true value
or settlement after fees, so that picking the luckiest of 37 is not taken for an edge. A probe whose
95% interval lies wholly above zero is flagged, and the command exits with 1. Where the real
market is shown to pay nothing (its resting orders' markouts in the validation report, a real
strategy's test), a flag means the fit offers an edge the real market lacks, and nothing learned
in it should be trusted until the fit is corrected. `--latency` runs the probes at a slower seat,
`--families maker,trend` runs only some. Given several scenarios, or a directory such as a twin
fit's `twins/`, the markets take them in turn, so that the check covers every recorded market's
twin and not one alone.

The validation report carries the real side of the makers' check: what a lot quoted 1, 3 and 8
ticks from the mid would have earned, measured alike on the recording and on the fit. A fit
leaves these statistics out of its target, so that they stay a check on it. Where the fit pays a
standing quote much more than the recording does, the makers' flags are the fit's, not the
market's. The measure ignores the queue and the quote's own effect on the others, and needs a mid:
a stretch where one side of the book is empty, such as a game in play with its makers away, goes
unmeasured, while a probe there may be the only liquidity, so a flag there needs a closer look.

On the fitted fifteen-minute question ([up_or_down.toml](../examples/scenarios/up_or_down.toml)),
100 markets a probe and 400 fresh ones, 18 seconds on four cores:

| each family's best, on fresh markets | made a market (tick-lots) | per lot traded |
|---|---:|---:|
| maker, 8 ticks either side | **+28.1 [+10.1, +46.0], flagged** | +1.39 |
| trend, 60 s against 600 s | −43.0 [−93.0, +7.0] | −1.24 |
| flow, 120 s at 80% | −57.3 [−104.0, −10.5] | −1.75 |
| reversal, 60 s against 600 s | −106.4 [−147.5, −65.2] | −3.91 |

That market was fitted to trades alone, with one market maker quoting around the average of trades,
and a maker quoting 8 cents either side of the mid catches the noise traders' large orders as they
sweep its thin book. Unless the real market pays such a quote as much, which the validation
report's standing quotes measure, it is the false edge this check exists for: the fitted market
should not be used to judge a maker until its book is fitted too.

### A learner in the check

Each probe follows one rule. A fitted market can pay what no one rule takes, but a strategy
trained there would find: a price that follows the value a few seconds late pays whoever reads,
from the fast traders' orders and the mid's path together, where it is going. So beside the
probes the sweep trains a small learner in the market itself
([learner.py](../analysis/src/crowdbook_analysis/learner.py)) and judges it the same way:

- **What it sees**, each decision, a second apart: the signed share of the volume that took over
  2, 20 and 120 seconds; the mid against its own averages over the same; the touch's imbalance;
  the spread; its position; the last trade against the mid. Anything a seat hearing the market
  could compute, and never the true value: the scenario's `[participant]` and `[learning]` are
  replaced by the probe's own seat, the probes' latency and limits, with no feed.
- **What it does**: three linear scores of those numbers. While the first is past 1 it takes
  toward its limit on that side, at the touch, as the probes' takers do, and holds what it has
  between; the second and third rest a bid and an offer at the best prices when above 0. It acts
  on its schedule only: at the decisions its own fills bring between, it keeps the quotes it still
  has where they are, places none afresh and takes nothing.
- **How it learns**: CMA-ES over the weights, from all of them 0, a strategy that never trades;
  each candidate plays 16 markets a generation, the same for every candidate of a generation (a
  seed runs the same one of several scenarios whoever plays it) and fresh each generation, for 20
  generations. It is scored on what it made after fees to its last decision, valued at the true
  value: what a calibrated value expects the same as a settlement, without the noise of a
  contract's jump to 0 or 100. The search's mean at the end is judged on as many fresh markets as
  each family's best, valued as the probes are, at the true value or settlement, and flagged the
  same way; the report shows its weights, which say what the market pays.

It trades through `crowdbook_learn`'s pool, the engine's Python module, found installed or in the
Release build beside the engine (`-DCROWDBOOK_BUILD_PYTHON=ON`, on by default where Python is
found). `--no-learner` leaves it out; `--learner-generations` and `--learner-markets` train it
harder, and a stronger learner finds more: a check that passes says the market pays nothing this
learner finds in this many markets, not nothing at all.

It has a known answer ([test_learner_probe.py](../python/tests/test_learner_probe.py), smaller):
a value that wanders, one maker quoting it, and three sharp traders who take when the book is two
ticks off it. With the maker 10 seconds behind the value, 100 markets a probe and 400 fresh ones,
27 seconds on four cores:

| on fresh markets | made a market (tick-lots) | lots a market |
|---|---:|---:|
| learner, 20 generations of 16 markets | **+40.8 [+31.1, +50.5], flagged** | 43.8 |
| flow, 5 s at 60% | **+32.0 [+23.1, +40.9], flagged** | 53.1 |
| trend, 60 s against 600 s | −26.4 [−35.6, −17.3] | 12.9 |

It takes on the flow of the last 2 and 20 seconds, as the best probe does, and on the spread and
the last trade beside it. With the maker quoting the value as it is, it learns to stay out and
makes nothing. In [the learning example](../examples/learning/market.toml), a market of a crowd
with makers who quote the trades, the best probes make +80 a market and the learner +260 [+253,
+267]: it quotes the touch on both sides and leans against its position, which no probe does.

On a twin fit's thirty-odd twins of one to four hours each, training and judging the learner on
140 fresh markets takes about six minutes on four cores.

## Judging a strategy in a fitted market

Once a market is fitted, and passes the no-false-edge check, a strategy is run in it as it would
trade the real one, through the learning pool's seat ([python/README.md](../python/README.md)):

```bash
uv run --project analysis crowdbook-draws my_spec.toml --report calibrated/report.json \
    --count 20 --out draws
```

```python
from pathlib import Path
from crowdbook_learn.evaluate import evaluate

result = evaluate(policy, Path("draws"), seeds=range(1, 401), scales=(1, 2, 4))
# or a twin fit's every recorded market: evaluate(policy, Path("fit/twins"), ...)
```

`crowdbook-draws` writes scenarios whose settings are drawn about the fit, each normal in the unit
box with its 68% interval's half width, doubled, as its spread; doubled because the intervals were
found about half as wide as they should be, except for a fit run with `--refits`, whose intervals
are used as they are (`--widen` sets it either way). The policy answers each decision with explicit orders
(`Pool.step_orders`: a bid, an offer, a quantity to take), so a strategy written for a real venue,
which names its prices, runs unchanged. The report: what it made a market, with its interval,
across the draws; its lots, as a maker and a taker; what it made a lot; its quotes' place in the queue, the share
of its quoted lots that filled and how long they waited; the markouts of its fills a
second, ten and a minute on, its adverse selection; and all of these at larger sizes on the same
seeds, its capacity. Set beside a paper trader's numbers on the real market, they say whether the
fitted market predicts it.

### Paper fills against the fit

A paper trader runs a strategy on a real book without sending orders, recording the quotes it would
have placed and the fills it would have had. Run the same strategy in the fitted market and
[fills.py](../analysis/src/crowdbook_analysis/fills.py) sets the two side by side, each figure
measured the same way, by the harness's own summary, with a 95% interval from resampling markets:
the share of quoted lots that filled, the lots ahead of a quote, the wait to fill, the share of
fills made resting, the lots a market, and a fill's worth 1, 10 and 60 seconds on. Figures whose
intervals do not overlap are where the fit trades the strategy unlike the real market.

```python
from crowdbook_analysis import fills
from crowdbook_learn.evaluate import run
text, apart = fills.compare(fills.read_paper("paper/"), run(policy, Path("fit/twins"), seeds=range(1, 401)))
```

A paper record is a tape of the book the strategy watched, with two more tables: `quotes`
(`market`, `time`, `side`, `price`, `size`, optionally `ahead`, the size shown at that price before
the quote joined it) and `fills` (`market`, `time`, `side`, `price`, `quantity`, `liquidity`,
`maker` or `taker`), times in seconds from each market's start, prices in the tape's units and
sizes in whole lots. Without `ahead`, the size ahead is read from the record's own book: the size
at the quote's price on its side in the last snapshot before it, as the harness counts it. A
record with fractional or missing sizes, or rows for markets its tape does not have, is refused.
`crowdbook-fills PAPER` prints a record's figures alone. Lots a market compare only where paper
markets and twins last alike, as twins of the recorded markets do.

Overlapping intervals are a weak test: the comparison names what is apart, and names what it
could not compare (a figure one side has no interval for), but it does not certify agreement.
Its known answers, on the learning example with 2-minute markets, 60 a side: a maker's record in
engine markets, written out as a paper record and read back, overlaps the same maker on fresh
seeds of those markets in every figure; set against the same maker where a quarter of the noise
traders trade, six of the eight figures are apart, among them the fill rate, the queue, the wait
and the markouts at 1 and 10 seconds.

## Datasets

`crowdbook-generate` ([generate.py](../analysis/src/crowdbook_analysis/generate.py)) makes many
markets from a spec, each with its settings drawn afresh from the ranges, uniformly on their
scales, and its own seed. It writes shards of 500 markets, each a tape in Parquet, and
`markets.csv` with every market's labels: its drawn settings, its seed, its outcome and, for each
group of traders, what it made after fees (`pnl_<group>`, at settlement or the true value) and the
lots it traded (`traded_<group>`). `dataset.toml` records the template, the ranges and the seeds,
so the dataset can be made again exactly; market *i* ran under seed `first_seed + i`.
`--fixed name=value,...` holds settings. `generate.read_dataset(DIR)` streams a dataset's markets
shard by shard, each with its labels. On four cores it makes about 40 five-minute markets a
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

## Decisions

- **One format, tapes, for recorded and simulated markets.** Why: a statistic computed by one
  piece of code on both sides cannot differ for reasons of code; a recording is converted once, by
  whoever holds it, and nothing in the engine knows where it came from. To change it: `tape.py`,
  with `read_tape` and `write_tape` kept each other's inverse (tested).
- **Intervals from resampling markets, not moments.** Why: moments within a market are alike, so
  counting them as independent makes every difference look significant; markets are the unit
  that repeats. The cost is wide intervals with few markets, and a bootstrap that understates the
  spread below about 50 markets (40 of 46 within 2 where 44 would be expected).
- **Distances in tolerances of both sides together, logarithms for scales.** Why: a target with
  thousands of markets is known precisely, and a synthetic market judged on a handful of seeds is
  not; summing both spreads asks the synthetic market to be within chance of the real one, not of
  itself. It is the fifteen-minute fit's rule, made general.
- **The calibration states whether it reached the target, and is checked on known settings.**
  Why: a fit stopped early sat on a compensating point with confident intervals; only the distance
  against chance and a recovery of known settings showed it.
- **A game is a kind of prediction market, not a new kind of market.** Why: settlement, prices of
  1 to 99, the probability fee curve, valuing at resolution and the learning environment all carry
  over unchanged; only the true value differs. Its value is the exact probability given the score,
  the time left and the strength as it is now, not integrated over how the strength may still
  drift: exact where nothing drifts (tested for calibration) and close otherwise.
- **The bettors' tilt is in log-odds.** Why: a favourite–longshot bias is a pull toward even that
  vanishes at 50 and cannot cross 0 or 100; a constant `bias` cannot express it.
- **The no-false-edge check runs each probe alone, at a lot an order.** Why: a probe that moves
  the market measures itself; alone and small, it measures the market. Each family's best is
  judged on fresh seeds because the best of many on the same seeds is partly luck.
- **Datasets are made by the Python package, not a new command.** Why: at about 80 ms a market on
  one core, half of it the simulation itself, a native writer would at most double the rate, and
  the spec, the labels and the format already live in Python. To change it: a `crowdbook run`
  option that writes a tape directly would remove the parsing.
