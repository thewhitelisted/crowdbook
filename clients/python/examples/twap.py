"""An execution algorithm: works the challenge's target evenly over the session (TWAP), with a
market order whenever it falls behind schedule.

    crowdbook serve examples/challenges/large_order.toml --seat broker
    python clients/python/examples/twap.py --seat broker
"""

import argparse

import crowdbook

SECOND = 1_000_000_000


class Twap(crowdbook.Bot):
    def __init__(self, interval=SECOND, finish_early=0.9):
        super().__init__()
        self.interval = interval
        self.finish_early = finish_early  # be done when this share of the session has passed

    def on_start(self):
        target = self.settings.scoring.target if self.settings.scoring else None
        if target is None:
            raise RuntimeError("this market sets no target to work")
        self.side, self.quantity = target.side, target.quantity
        self.start = self.now
        self.initial = self.ledger.position
        self.wake_after(self.interval)

    def on_wakeup(self, tag):
        self.wake_after(self.interval)
        if self.book.phase != "continuous":
            return  # its market orders cannot wait for an auction; the order waits
        span = self.finish_early * (self.settings.duration - self.start)
        due = min(self.quantity, round(self.quantity * (self.now - self.start) / span))
        sign = 1 if self.side == "buy" else -1
        # Done so far, plus orders still in flight.
        done = sign * (self.ledger.position - self.initial) + sum(
            order.leaves for order in self.ledger.orders.values() if order.side == self.side)
        size = min(due - done, self.settings.account.max_order_quantity)
        if size > 0:
            (self.buy_market if self.side == "buy" else self.sell_market)(size)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7878)
    parser.add_argument("--seat", default="you")
    parser.add_argument("--token")
    args = parser.parse_args()
    end = crowdbook.run(Twap(), args.host, args.port, args.seat, args.token)
    print(f"twap done: {end}")


if __name__ == "__main__":
    main()
