"""A trend follower: buys when a fast average of trade prices runs ahead of a slow one and sells
when it falls behind, within a position limit.

    crowdbook serve examples/scenarios/playable.toml --seat trend
    python clients/python/examples/momentum.py --seat trend
"""

import argparse

import crowdbook

MILLISECOND = 1_000_000


class Momentum(crowdbook.Bot):
    def __init__(self, fast=0.3, slow=0.05, threshold=0.5, size=1, max_position=10,
                 interval=500 * MILLISECOND):
        super().__init__()
        self.fast_weight, self.slow_weight = fast, slow  # per trade
        self.threshold = threshold  # ticks
        self.size = size
        self.max_position = max_position
        self.interval = interval
        self.fast = self.slow = None

    def on_start(self):
        self.wake_after(self.interval)

    def on_trade(self, trade):
        if self.fast is None:
            self.fast = self.slow = float(trade.price)
        self.fast += self.fast_weight * (trade.price - self.fast)
        self.slow += self.slow_weight * (trade.price - self.slow)

    def on_wakeup(self, tag):
        self.wake_after(self.interval)
        if self.fast is None or self.book.phase != "continuous":
            return  # market orders cannot wait for an auction
        # Orders in flight count, so that a burst of decisions cannot overshoot the limit.
        held = (self.ledger.position + self.ledger.open_quantity("buy")
                - self.ledger.open_quantity("sell"))
        lead = self.fast - self.slow
        if lead > self.threshold and held + self.size <= self.max_position:
            self.buy_market(self.size)
        elif lead < -self.threshold and held - self.size >= -self.max_position:
            self.sell_market(self.size)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7878)
    parser.add_argument("--seat", default="you")
    parser.add_argument("--token")
    args = parser.parse_args()
    end = crowdbook.run(Momentum(), args.host, args.port, args.seat, args.token)
    print(f"momentum done: {end}")


if __name__ == "__main__":
    main()
