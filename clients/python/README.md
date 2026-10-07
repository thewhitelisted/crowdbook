# crowdbook for Python

Trade in a market served by `crowdbook serve`, from Python, by hand or with a bot. The package
uses only the standard library and needs Python 3.11 or later.

```bash
pip install ./clients/python
```

A bot overrides the callbacks it needs and acts through its methods:

```python
import crowdbook

class DipBuyer(crowdbook.Bot):
    def on_trade(self, trade):
        if trade.price < self.settings.reference_price - 5 and self.ledger.position < 10:
            self.buy_market(1)

end = crowdbook.run(DipBuyer(), host="127.0.0.1", port=7878, seat="alice")
print(end.pnl)
```

- **Callbacks:** `on_start`, `on_wakeup`, `on_trade`, `on_top`, `on_depth`, `on_accepted`,
  `on_rejected`, `on_modified`, `on_filled`, `on_cancelled`, `on_error` and `on_end`, each with
  the message as it arrived.
- **Actions:** `buy` and `sell` at a limit price, with a time in force (`good-till-cancel`,
  `immediate-or-cancel` or `post-only`), `buy_market` and `sell_market`, `cancel`, `cancel_all`
  and `modify`. Orders are named by ids the bot gives them, so a bot can cancel an order before
  it is acknowledged. `wake_after` asks for `on_wakeup` after a stretch of the market's time.
- **State:** `ledger` holds the seat's cash, position, fees and orders, built from what the bot
  sent and heard back; `book` the best prices, the depth and the last trade; `settings` the
  market's length, reference price, fees, latency and the seat's limits; `now` the market's time
  in nanoseconds.

Lower down, `crowdbook.Connection` sends and receives the protocol's messages, defined in
`crowdbook.protocol`; [docs/protocol.md](../../docs/protocol.md) specifies them.

[examples](examples) has a market maker, a trend follower and an execution algorithm that works a
challenge's target evenly over the session. Each runs against a served market:

```bash
./build/release/apps/crowdbook serve examples/scenarios/playable.toml --seat maker --seat trend
python clients/python/examples/market_maker.py --seat maker
python clients/python/examples/momentum.py --seat trend
```

A seat in a scored challenge gets the challenge's name, briefing and scoring rules in
`settings.challenge` and `settings.scoring`, and its score at the end, in points, in the `End`
that `run` returns.

The tests run with `python -m unittest discover -s tests` from this directory; the ones that
trade against a real server need `CROWDBOOK` set to the `crowdbook` command, as CTest does.
