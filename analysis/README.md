# crowdbook-analysis

The commands that fit [crowdbook](https://github.com/thewhitelisted/crowdbook)'s simulated markets
to a recorded one and say whether the fit can be trusted for testing strategies. All but
`crowdbook-tape` run the `crowdbook` command, which comes with `crowdbook-engine`:
`pip install crowdbook-engine crowdbook-analysis`.

| command | does |
|---|---|
| `crowdbook-assess` | from a recording to a verdict: the fit, a check that it reaches the recording within chance, and a check that the fitted market pays no simple or learned strategy |
| `crowdbook-calibrate` | fits a scenario's settings to a recording, with intervals |
| `crowdbook-validate` | about 50 statistics of a recording against simulated markets |
| `crowdbook-sweep` | the check for false edges alone |
| `crowdbook-draws` | scenarios drawn from a fit's uncertainty, to test a strategy across |
| `crowdbook-generate` | labelled synthetic markets, in Parquet shards |
| `crowdbook-tape` | a recording in plain CSV, quotes and trades, turned into a tape |

[The tutorial](https://github.com/thewhitelisted/crowdbook/blob/main/docs/tutorial.md) goes from a
recording to a tested strategy in about fifteen minutes, and
[validation.md](https://github.com/thewhitelisted/crowdbook/blob/main/docs/validation.md) is the
reference. Licensed under [PolyForm Shield 1.0.0](https://github.com/thewhitelisted/crowdbook/blob/main/LICENSE).
