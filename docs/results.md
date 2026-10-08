# Results

What the experiments with crowdbook found. Every number and chart here comes from the scripts in
[`analysis/`](../analysis), run against the Release build. Every run is seeded, so the same
commands reproduce them exactly:

```bash
cmake --workflow --preset release
uv run --project analysis crowdbook-facts         # about 12 minutes on 10 cores
uv run --project analysis crowdbook-experiments   # a few seconds
uv run --project analysis crowdbook-large-orders  # about 2 minutes
uv run --project analysis crowdbook-trading-day   # a few seconds
```

Uncertainties are standard errors across independent runs. Prices are in ticks, and PnL is in
ticks per lot traded.

## The markets

The stylized facts come from two markets built around the same thousand zero-intelligence traders
(Farmer, Patelli and Zovko, 2005). Each of those traders sends 0.4 limit orders and 0.1 market
orders a second, of 1 to 10 lots, on random sides. Each resting order is cancelled after an
exponential lifetime of 5 seconds on average.

- **Mixed market** ([large_market.toml](../examples/scenarios/large_market.toml)), with 1,041
  agents: the thousand noise traders, plus an Avellaneda–Stoikov market maker on a 20 µs
  connection, twenty trend followers, and twenty informed traders. The informed traders see the
  asset's true value with noise. That value is a random walk of 0.2 ticks per √second.
- **Noise traders only** ([large_noise.toml](../examples/scenarios/large_noise.toml)): the
  thousand noise traders on their own.

Each market ran for one simulated day under each of four seeds, sampling prices every second. It
also ran for one simulated hour under each seed with its event log, for spreads and price impact.
One simulated day of the mixed market is 17 million trades and takes 41 seconds on one core.

## What emerges

| Stylized fact of real markets (Cont, 2001) | Mixed market | Noise traders only |
|---|---|---|
| Tight spread | Yes: 1.58 ticks on average | Yes: 1.51 ticks |
| Mid price reverts at short horizons | Yes: lag-1 autocorrelation of 1 s returns is −0.38 | Yes: −0.41 |
| Returns uncorrelated beyond a few minutes | Not quite: −0.12 at 5 min, as the price is pulled back to value | Yes |
| Fat tails | Only around 10 s: excess kurtosis +0.86, gone by 1 min | No |
| Volatility clustering | Only around 10 s: absolute-return autocorrelation +0.21 at lag 1 | Barely: +0.04 |
| Price anchored to value at long horizons | Yes: volatility over an hour equals the true value's | No value to anchor to |
| Uninformed impact decays, informed impact stays | Yes | — |

In these two markets, two facts of real markets are missing (the [memory market](#memory-what-makes-volatility-cluster) brings them in). Fat tails and volatility clustering appear here only over
seconds, and fade within a minute. Real markets show fat tails at intraday and daily horizons, and
volatility clustering that lasts for weeks (Cont, 2001). Every agent here acts at a constant
average rate, and none remembers more than a few seconds of prices. Nothing can make a busy hour
follow a busy hour.

### Prices bounce at short horizons

![Volatility per square root of a second at horizons from one second to an hour](images/volatility_signature.png)

For a random walk, volatility per √second is the same at every horizon. Here it falls by a factor
of six in the mixed market, from 1.24 ticks at one second to 0.20 at an hour. The mechanism shows
in the [price impact](#who-moves-prices) below:
1. When a market order empties the best price level, the mid jumps.
2. Within a second, new limit orders land inside the widened spread and pull it back.

Real transaction prices give the same falling curve in a volatility signature plot (Andersen et
al., 2000), where the bid–ask bounce plays the part of the refilling book.

At long horizons the two markets part ways. The noise traders alone wander at 0.36 ticks per
√second. In the mixed market, the informed traders hold the price to the true value, so it settles
at the value's own 0.20.

| Market | Trades per second | Mean spread | Volatility at 1 s | 10 s | 1 min | 10 min | 1 h |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mixed market | 200 | 1.58 | 1.24 | 0.57 | 0.29 | 0.21 | 0.20 ± 0.01 |
| Noise traders only | 181 | 1.51 | 1.32 | 0.58 | 0.41 | 0.37 | 0.36 ± 0.04 |

![Share of time the spread was one, two, three or four ticks wide](images/spreads.png)

### Trend followers make bursts; informed traders anchor the price

![Tails of 10-second and one-minute returns against a normal distribution](images/return_tails.png)

![Autocorrelation of 10-second returns and of their absolute values](images/autocorrelation.png)

| Market | Excess kurtosis, 10 s | Excess kurtosis, 1 min | Absolute-return autocorrelation, 10 s | Absolute-return autocorrelation, 1 min | Volatility at 1 h |
|---|---:|---:|---:|---:|---:|
| Mixed market | +0.86 ± 0.05 | −0.10 ± 0.04 | +0.212 ± 0.004 | +0.067 ± 0.010 | 0.20 ± 0.01 |
| … without the market maker | +0.73 ± 0.04 | −0.02 ± 0.07 | +0.248 ± 0.007 | +0.083 ± 0.015 | 0.20 ± 0.01 |
| … without the trend followers | −0.26 ± 0.02 | −0.15 ± 0.05 | +0.098 ± 0.005 | +0.021 ± 0.013 | 0.20 ± 0.01 |
| … without the informed traders | +0.55 ± 0.04 | −0.48 ± 0.02 | −0.136 ± 0.006 | +0.015 ± 0.009 | 0.36 ± 0.04 |
| Noise traders only | −0.31 ± 0.02 | −0.05 ± 0.07 | +0.037 ± 0.008 | +0.000 ± 0.027 | 0.36 ± 0.04 |

Leaving out one group at a time shows which agents shape the mixed market:

- **Trend followers** cause the fat tails and most of the clustering at 10 seconds. A move of a
  tick or so sets all twenty buying together, which extends the move and adds a burst of
  volatility. Without them, 10-second returns are thinner-tailed than normal, like the noise
  market's.
- **Informed traders** anchor the price. Without them, long-run volatility returns to the noise
  market's 0.36. The trend followers' runs then go uncorrected, and the market behaves strangely:
  one-minute returns are much thinner-tailed than normal (−0.48), and absolute returns are
  anti-correlated at 10 seconds. That regime deserves a closer look.
- **The market maker** changes little. It trades 63 lots a second, against 1,121 for the noise
  traders.

Even the mixed market's 10-second tails are mild. Real intraday returns are far more heavy-tailed,
with excess kurtosis often in the tens (Cont, 2001).

### Who makes money

| Trader | Lots traded per second | PnL per lot, positions at the true value |
|---|---:|---:|
| Market maker | 63.3 | +0.615 ± 0.006 |
| Noise traders | 1,121.2 | +0.064 ± 0.005 |
| Trend followers | 48.1 | −3.275 ± 0.009 |
| Informed traders | 33.8 | +1.39 ± 0.12 |

Trading is zero-sum, and the trend followers pay for everyone else. They lose 3.3 ticks on every
lot. That funds the informed traders and the market maker, and even leaves the noise traders
slightly ahead.

### Who moves prices

![Mid move after aggressive orders from noise traders, trend followers and informed traders, from a millisecond to two minutes](images/impact.png)

The chart measures how far the mid moves in an order's direction after it takes liquidity. The
averages run over four hours of the mixed market, with errors taken across the four runs.

| Trader | Aggressive orders | 1 ms | 100 ms | 1 s | 10 s | 1 min | 2 min |
|---|---:|---:|---:|---:|---:|---:|---:|
| Noise | 1,443,477 | +0.12 | +0.07 | +0.02 | +0.00 | +0.01 | +0.01 |
| Trend | 128,238 | +0.07 | +0.06 | +0.09 ± 0.03 | −2.17 ± 0.05 | −2.15 ± 0.09 | −2.19 ± 0.05 |
| Informed | 107,279 | +0.24 | +0.62 | +1.15 ± 0.01 | +2.03 ± 0.03 | +1.90 ± 0.03 | +1.87 ± 0.03 |

- **Noise orders** have purely transient impact. They move the mid a tenth of a tick, and the
  move is gone within a second.
- **Informed orders** have permanent impact. The mid keeps moving their way for ten seconds as
  the other informed traders reach the same conclusion, and stays there, about two ticks on.
- **Trend followers' orders** arrive as a move ends. Within ten seconds the price has gone two
  ticks against them, which is where their losses come from.

This is the textbook split between informed and uninformed flow (Kyle, 1985; Glosten and Milgrom,
1985). Here it comes out of the agents' behaviour; nothing in the model assumes it.

### Does the number of agents matter?

![Volatility signature of the same total order flow sent by 10, 100 or 1,000 noise traders](images/crowd_size.png)

The same total flow (400 limit and 100 market orders a second) was split among 10, 100 or 1,000
noise traders, with unlimited positions, for four simulated days each:

| Noise traders | Orders per trader per second | Trades per second | Mean spread | Lag-1 autocorrelation at 1 s | Volatility at 1 s | Volatility at 10 min | Excess kurtosis at 1 min |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 50 | 152.9 | 1.398 | −0.405 | 1.117 | 0.351 ± 0.015 | −0.13 ± 0.04 |
| 100 | 5 | 178.6 | 1.502 | −0.412 | 1.299 | 0.362 ± 0.010 | −0.18 ± 0.06 |
| 1,000 | 0.5 | 181.3 | 1.512 | −0.412 | 1.320 | 0.366 ± 0.011 | −0.05 ± 0.07 |

From 100 traders up, headcount makes almost no difference: spread and volatility agree within 2%.
Independent traders acting at random times add up to one random stream with the summed rate,
however the flow is divided.

With 10 traders, one difference appears, and it is mechanical: self-trade prevention. An agent's
market order meets its own resting order and is cancelled. A ten-minute run of each crowd (the
scenarios are in `stylized_facts.py`) with `--log-only trade,cancelled` counts 17 such
cancellations a second with 10 traders, 1.8 with 100 and 0.2 with 1,000. So 16% fewer trades
print, the spread is tighter and 1-second volatility is lower.

What makes market behaviour is what agents react to, not how many there are. The thousand
independent noise traders show no fat tails at all. Twenty trend followers reacting to the same
price do.

### Memory: what makes volatility cluster

[memory_market.toml](../examples/scenarios/memory_market.toml) is the mixed market with news
(a 6-tick jump in the true value every 30 minutes on average) and noise traders who react to the
market: their pace follows recent activity against its usual level, and when prices have been
jumping more than usual they place their limit orders further from the best prices. Its informed
traders have larger limits, 10,000 lots each, so they never run out of room.

![Autocorrelation of absolute one-minute returns at lags up to two hours](images/clustering.png)

| Market | Excess kurtosis, 1 min | Clustering at 1 min | 15 min | 1 h | 2 h |
|---|---:|---:|---:|---:|---:|
| Memory market | +8.5 ± 0.7 | +0.24 | +0.18 | +0.08 | −0.00 |
| … without news | +6.3 ± 1.3 | +0.31 | +0.21 | +0.09 | +0.02 |
| … without the activity response | +5.8 ± 1.2 | +0.22 | +0.10 | +0.06 | +0.00 |
| … without the volatility response | +2.6 ± 0.5 | +0.03 | +0.01 | +0.01 | −0.01 |
| … with fifty adaptive traders | +2.8 ± 0.7 | +0.05 | +0.03 | +0.01 | +0.01 |
| Mixed market, no memory | −0.1 | +0.07 | +0.01 | −0.01 | −0.02 |

"Clustering" is the autocorrelation of absolute one-minute returns at that lag; the 95% band for no
correlation is ±0.03. Tails thin with the horizon, as in real markets: excess kurtosis is +19 at
10 seconds, +8.5 at a minute and +1.7 at five minutes.

- **Liquidity feedback is the cause.** Without the volatility response the clustering is gone.
  Jumpy prices make the noise traders stand back, a thinner book makes the next jump bigger, and
  volatile spells last about an hour.
- **Activity feedback alone does nothing:** a thousand traders already trade hundreds of times a
  second, so activity barely fluctuates (trades per minute varied by about 4%). On top of the
  liquidity feedback it lengthens the clustering.
- **News adds fat tails but not clustering.**
- **Adaptive traders cancel it.** Fifty traders switching between value and trend strategies
  (Brock and Hommes, 1998) wipe the clustering out; why is still an open question.
- Clustering fades by two hours, where in real markets it lasts weeks. Splitting the noise
  traders across memories of a minute, ten minutes and an hour did not stretch it further than
  judging "usual" over a day did.

## Large orders

Much of the volume in real markets comes from parent orders that brokers' algorithms cut into many
small child orders over minutes or hours. Two of the best-documented regularities of market
microstructure follow from that (Bouchaud, Farmer and Lillo, 2009): the signs of market orders stay
correlated over thousands of orders, and a parent order moves the price roughly as the square root
of its size.

[large_orders.toml](../examples/scenarios/large_orders.toml) has a hundred brokers working parents
among the thousand noise traders. Each broker takes one parent at a time, after a pause of 20
seconds on average, with a random side and a size from a Pareto tail, P(size > q) = (15 / q)^1.5,
capped at 5,000 lots. It trades the parent with a 3-lot market order every 200 ms, so a parent of
a hundred lots takes seven seconds and one of five thousand almost six minutes. The noise traders
send few market orders of their own (0.02 a second each), so three quarters of all market orders
are children of parents. Each child carries its parent's id into the event log, which is how the
analysis puts parents back together.

The market has no informed traders. In a trial in the mixed market, whose informed traders see the
true value and hold thousands of lots, the brokers' parents, which carry no information, moved the
price by less than half a tick on average at every size: the informed traders absorbed them.

Each case ran for an hour under 16 seeds, with its event log.

### The signs of market orders remember

![Autocorrelation of the signs of market orders against the lag between them](images/sign_memory.png)

| Parent sizes | Orders | Lag 10 | Lag 100 | Lag 1,000 | Fitted exponent | Predicted |
|---|---|---:|---:|---:|---:|---:|
| Tail 1.5 | brokers' | +0.071 | +0.040 | +0.010 | 0.56 ± 0.03 | 0.5 |
| Tail 1.5 | all | +0.010 | +0.027 | +0.007 | 0.53 ± 0.02 | 0.5 |
| Tail 2.5 | brokers' | +0.135 | +0.012 | +0.001 | 1.53 ± 0.14 | 1.5 |
| Tail 2.5 | all | +0.055 | +0.011 | +0.000 | 1.32 ± 0.07 | 1.5 |

Lags count market orders. Standard errors of the correlations are about 0.001; the exponent of the
power-law decay is fitted over lags 32 to 2,500 (to a few hundred for the thinner tail, beyond
which its correlation is lost in noise), with its error from resampling whole runs.

- **The decay follows the tail of parent sizes.** Lillo, Mike and Farmer (2005) showed that when
  parent sizes have a Pareto tail with exponent α and many parents are worked at once, the
  correlation decays as lag^−(α−1). The simulation agrees with both tails it was given. With
  α = 1.5 the correlations decay so slowly that their sum diverges: long memory, as in real order
  flow. With α = 2.5 they die out within a few hundred orders.
- **The noise traders' orders dilute the correlation but leave its decay.** Their signs are
  independent, so the correlation of all orders is smaller, but it falls off at the same rate.
- **Consecutive orders are not correlated.** About a dozen parents are worked at once, each
  sending a child every 200 ms, so the next child of the same parent comes about a dozen orders
  later. In real markets the correlation is strongest at lag 1; their parents are worked more
  unevenly than this fixed-pace TWAP.

### Impact: the square root needs a book that remembers

![Mean move of the mid in a parent's direction against the parent's size](images/parent_impact.png)

| Noise traders' orders rest | Parents | 15–30 lots | 150–300 lots | 1,500–5,000 lots | Fitted exponent |
|---|---:|---:|---:|---:|---:|
| 5 s | 188,021 | +0.10 ± 0.00 | +0.56 ± 0.02 | +7.4 ± 0.6 | 0.90 ± 0.02 |
| 50 s | 188,021 | +0.06 ± 0.00 | +0.14 ± 0.01 | +1.1 ± 0.1 | 0.60 ± 0.02 |
| 500 s | 188,021 | +0.05 ± 0.00 | +0.09 ± 0.01 | +0.20 ± 0.05 | 0.20 ± 0.08 |

Impact is the mid's move in the parent's direction from just before its first child to its last
fill, in ticks. The exponent is fitted to the mean impact in size bins holding at least 50 parents.
The brokers draw the same parents in all three markets, so the rows differ only in the market.

- **With the noise traders' usual 5-second orders, impact is almost linear in size** (exponent
  0.90 ± 0.02), not the square root of real markets.
- **The longer resting orders last, the more concave impact becomes:** an exponent of
  0.60 ± 0.02, near the square root, when they rest for 50 seconds, and 0.20 ± 0.08 at 500
  seconds.
- This is what theories of latent liquidity predict (Tóth et al., 2011; Donier et al., 2015). A
  book that remembers where the price has been holds orders a parent has to eat through, and
  they thin out toward the current price, which bends impact into a square root. A book that
  renews itself faster than the parent trades meets every child with fresh orders around the
  current price, so each child moves the price about as far as the last, and impact adds up
  linearly. Here a parent of a thousand lots takes about a minute: longer than a 5-second book
  remembers, shorter than a 500-second one.
- Longer-lived orders also make the book deeper, since the noise traders keep placing orders at the
  same rate, so these runs change the book's depth and its memory together. Separating the two,
  and measuring how much of the impact stays after a parent ends, are open.

## The trading day

[trading_day.toml](../examples/scenarios/trading_day.toml) trades the way a stock exchange's day
goes: a one-minute opening auction, continuous trading, a one-minute closing auction, and halts
when a trade lands more than 15 ticks from the last auction's price. Sixty noise traders follow a
U-shaped activity curve, 1.8 times their average pace at the open and the close and 0.6 times at
midday; a market maker, five trend followers, five informed traders and five brokers working
parents by the same curve (VWAP) trade among them. The curve is an assumption, stated in the
scenario, like the U shape of real exchanges' volume it imitates; what follows from it is
measured. Each day ran for six and a half hours under 16 seeds, with the curve and with a flat
one, and the continuous session was cut into thirteen half hours.

![Volume, one-minute volatility and the spread through the day, against each one's average](images/trading_day.png)

| activity | measure | first third | middle third | last third | first / middle |
|---|---|---:|---:|---:|---:|
| U-shaped (2.0) | volume (lots a minute) | 1,328 | 844 | 1,398 | 1.57 |
| U-shaped (2.0) | volatility (ticks, one minute) | 2.43 | 3.23 | 2.28 | 0.75 |
| U-shaped (2.0) | spread (ticks) | 0.89 | 0.91 | 0.92 | 0.98 |
| flat | volume (lots a minute) | 1,230 | 1,181 | 1,202 | 1.04 |
| flat | volatility (ticks, one minute) | 2.55 | 2.36 | 2.54 | 1.08 |
| flat | spread (ticks) | 0.89 | 0.95 | 0.90 | 0.94 |

- **Volume takes the curve's shape**, as it must: the busy hours trade half as much again as
  midday.
- **Volatility takes the opposite shape.** It is a quarter lower in the busy hours than at
  midday, where on real exchanges it is highest at the open. The curve brings more noise traders'
  limit orders as well as their market orders, so the book is deepest when the market is busiest,
  and the trend followers', informed traders' and brokers' orders, which do not follow the curve,
  move the price least then. An activity curve alone does not make the open volatile; in real
  markets that comes with the news that piled up overnight, which this market does not have.
- **Spreads hardly move** through the day, within a few percent either way.
- **The auctions are small.** In a full day of this market the two auctions trade under 0.2% of
  the volume; real closing auctions trade around a tenth, because index funds and brokers send
  their orders for the close. Here only noise traders send auction orders, and only as many as
  the market orders they would have sent in that minute.
- **Halts follow the drift.** With a 15-tick band the market halted twelve times in one day, each
  time restarting from the halt's auction price, as the true value wandered away from where the
  day began.

## Market-maker experiments

These use a smaller market, so many seeds are cheap. Each point averages 32 runs of 60 seconds.
Each run has twenty noise traders with one market order a second each, plus an Avellaneda–Stoikov
market maker (Avellaneda and Stoikov, 2008).

### Self-impact: skewing quotes moves the market

![Market maker PnL per lot against its quote skew, for thin, medium and deep noise liquidity](images/maker_self_impact.png)

The noise traders anchor their limit prices on the best quotes, which are often the market
maker's own. When it skews its quotes against its inventory, it moves the market against itself.
PnL per lot falls as the skew per lot of inventory (γσ²τ) grows. It falls faster the less
liquidity the noise traders post themselves (0.5, 1 or 2 limit orders a second each):

| Skew per lot | Thin | Medium | Deep |
|---:|---:|---:|---:|
| 0.022 | −0.36 ± 0.45 | +1.70 ± 0.12 | +1.52 ± 0.22 |
| 0.045 | −1.31 ± 0.46 | +1.54 ± 0.09 | +1.48 ± 0.22 |
| 0.09 | −2.62 ± 0.54 | +1.31 ± 0.10 | +1.39 ± 0.23 |
| 0.18 | −4.59 ± 0.68 | +0.92 ± 0.12 | +1.17 ± 0.18 |
| 0.36 | −11.4 ± 3.7 | +0.22 ± 0.14 | +0.75 ± 0.30 |
| 0.72 | −86 ± 20 | −1.30 ± 0.40 | −0.12 ± 0.43 |

The default skew of 0.045 sits in the safe region for medium and deep markets. In a thin market
the maker loses at every skew: its own quotes are the market.

### Who pays for informed trading

![PnL per lot of the market maker, noise traders and informed traders as informed traders are added, for wide quotes and quotes at the touch](images/maker_adverse_selection.png)

The textbook expects informed traders to cost the market maker (Glosten and Milgrom, 1985). Here
its PnL per lot does not fall as informed traders are added. That holds whether it quotes wide
(intensity k = 0.3, about 3 ticks from its fair price) or at the touch (k = 1.0). The true value is
a random walk of 4 ticks per √second.

| Maker's quotes | Informed traders | Maker at 20 µs | Maker at 5 ms | Noise traders | Informed traders |
|---|---:|---:|---:|---:|---:|
| Wide | 0 | +1.72 ± 0.14 | +1.81 ± 0.14 | −0.22 ± 0.02 | — |
| Wide | 2 | +2.29 ± 0.03 | +2.26 ± 0.03 | −1.08 ± 0.10 | +2.96 ± 0.47 |
| Wide | 20 | +2.36 ± 0.03 | +2.25 ± 0.03 | −1.18 ± 0.09 | +1.61 ± 0.31 |
| At the touch | 0 | +0.56 ± 0.02 | +0.59 ± 0.02 | −0.18 ± 0.01 | — |
| At the touch | 2 | +0.62 ± 0.02 | +0.56 ± 0.02 | −0.96 ± 0.17 | +2.50 ± 0.62 |
| At the touch | 20 | +0.62 ± 0.01 | +0.47 ± 0.01 | −0.84 ± 0.15 | +1.03 ± 0.34 |

The maker is still adversely selected. The markouts below show how the mid moves in the maker's
favour after each of its fills, split by who took the other side, at a 20 µs connection:

| Maker's quotes | Informed traders | Other side | Share of maker's lots | 1 ms | 1 s | 10 s |
|---|---:|---|---:|---:|---:|---:|
| Wide | 0 | Noise | 100% | +2.33 | +1.94 ± 0.05 | +1.36 ± 0.15 |
| Wide | 20 | Informed | 23% | +2.67 | −0.99 ± 0.09 | −0.90 ± 0.32 |
| Wide | 20 | Noise | 77% | +2.86 | +3.33 ± 0.04 | +3.29 ± 0.09 |
| At the touch | 0 | Noise | 100% | +0.97 | +0.54 ± 0.02 | +0.51 ± 0.02 |
| At the touch | 20 | Informed | 37% | +0.97 | −0.85 ± 0.05 | −0.89 ± 0.17 |
| At the touch | 20 | Noise | 63% | +1.09 | +1.49 ± 0.03 | +1.45 ± 0.10 |

Within a second, the maker's fills against informed traders are losing about 0.9 ticks per lot.
But the same informed traders pull the price back to value after noise orders push it away. The
maker took the other side of those noise orders, so it collects that reversion. At the touch, its
fills against noise traders earn +1.45 a lot with informed traders around, against +0.51 without
them.

The two effects roughly cancel, and the informed traders more than double the maker's volume. The noise
traders pay for both. Glosten–Milgrom has a single liquidity provider. Here a crowd posts liquidity
too, and informed traders also correct the noise traders' mistakes.

Two smaller effects show in the table:
- A slow maker suffers a little more. At the touch on a 5 ms connection, its PnL per lot falls
  from +0.59 to +0.47 as informed traders are added; at 20 µs it does not fall.
- Informed traders compete away their own edge: profit per lot falls from 2.96 with two of them
  to 1.61 with twenty.

`crowdbook-experiments` prints the full tables, including the 500 µs latency and every count of
informed traders.

## What the analysis caught

Measuring the markets found four modelling problems, each fixed before the results above:

1. **Cancellation per trader.** Zero-intelligence traders first cancelled their own orders at a
   fixed rate per trader. The book grew without limit, from 4,500 to 12,500 resting orders in ten
   minutes, and the stale orders pinned the price for hours. Each order now has its own
   exponential lifetime, and the book holds steady near a thousand orders.
2. **Lockstep timers.** All twenty informed traders woke at exactly the same instants, so they hit
   the book as one burst five times a second. The impact measurement showed the price still moving
   within a millisecond of an informed order. Timers now start at a random point in their first
   interval.
3. **Informed traders out of capacity.** Holding the price to the true value means absorbing the
   noise traders' net order flow, which wanders like a random walk. With limits of 100 lots each,
   the informed traders filled up within half an hour. By the end of a day the price was up to 780
   ticks from the value. With those limits the mixed market showed fat tails at one minute (excess
   kurtosis +1.27 ± 0.30), but they were an artifact of the drifting price. With larger limits and a calmer
   value, they are gone.
4. **Idle agents.** Once the informed traders anchored the price, the trend followers' two-tick
   trends almost never formed, and the market maker's quotes, three ticks out, were almost never
   reached. Both were retuned until every type of agent trades. The settings are in the scenario
   file.

The fourth is a choice, not a bug. The results above hold for these settings, and other settings
give other markets. Making that easy to explore is the point of the toolkit.

## Limitations and next steps

- Volatility clusters for about an hour in the memory market, where in real markets it lasts
  weeks; no agent here remembers longer than a day.
- Each scenario is one point in a large parameter space, chosen so that every type of agent
  trades. The scripts make sweeps cheap.
- None of these markets is calibrated to real data yet; a scorecard comparing the same statistics
  on real order books is on the roadmap.
- One instrument, no hidden orders, and an exchange that takes no time to process a message.
- The trading day's open is no more volatile than its midday: nothing here brings news at the
  open, and nobody but the noise traders trades in the auctions.
- Impact and markouts are measured on the mid, so they miss changes in depth behind the best
  prices.

## References

- Andersen, T., Bollerslev, T., Diebold, F. and Labys, P. (2000). Great realizations. *Risk* 13,
  105–108.
- Avellaneda, M. and Stoikov, S. (2008). High-frequency trading in a limit order book.
  *Quantitative Finance* 8(3), 217–224.
- Bouchaud, J.-P., Farmer, J. D. and Lillo, F. (2009). How markets slowly digest changes in supply
  and demand. In *Handbook of Financial Markets: Dynamics and Evolution*, 57–160. North-Holland.
- Brock, W. and Hommes, C. (1998). Heterogeneous beliefs and routes to chaos in a simple asset
  pricing model. *Journal of Economic Dynamics and Control* 22(8–9), 1235–1274.
- Cont, R. (2001). Empirical properties of asset returns: stylized facts and statistical issues.
  *Quantitative Finance* 1(2), 223–236.
- Donier, J., Bonart, J., Mastromatteo, I. and Bouchaud, J.-P. (2015). A fully consistent,
  minimal model for non-linear market impact. *Quantitative Finance* 15(7), 1109–1121.
- Farmer, J. D., Patelli, P. and Zovko, I. (2005). The predictive power of zero intelligence in
  financial markets. *PNAS* 102(6), 2254–2259.
- Glosten, L. and Milgrom, P. (1985). Bid, ask and transaction prices in a specialist market with
  heterogeneously informed traders. *Journal of Financial Economics* 14(1), 71–100.
- Kyle, A. (1985). Continuous auctions and insider trading. *Econometrica* 53(6), 1315–1335.
- Lillo, F., Mike, S. and Farmer, J. D. (2005). Theory for long memory in supply and demand.
  *Physical Review E* 71, 066122.
- Tóth, B., Lempérière, Y., Deremble, C., de Lataillade, J., Kockelkoren, J. and Bouchaud, J.-P.
  (2011). Anomalous price impact and the critical nature of liquidity in financial markets.
  *Physical Review X* 1, 021006.
