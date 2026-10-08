"""A market maker: one bid and one offer around the middle of the market, leaning against its
inventory, requoted on a timer.

    crowdbook serve examples/scenarios/playable.toml --seat maker
    python clients/python/examples/market_maker.py --seat maker
"""

import argparse

import crowdbook

MILLISECOND = 1_000_000


class MarketMaker(crowdbook.Bot):
    def __init__(self, half_spread=2, size=2, skew=0.2, requote=250 * MILLISECOND):
        super().__init__()
        self.half_spread = half_spread  # ticks either side of the middle
        self.size = size
        self.skew = skew  # ticks the quotes lean per lot held
        self.requote = requote

    def on_start(self):
        self.quote()

    def on_wakeup(self, tag):
        self.quote()

    def quote(self):
        self.wake_after(self.requote)
        if self.book.phase != "continuous":
            return  # post-only quotes cannot rest in an auction, and nothing trades after the close
        self.cancel_all()
        middle = self.book.mid or self.book.last_price or self.settings.reference_price
        # Lean against the position: holding shares lowers both quotes, to sell more and buy less.
        center = round(middle - self.skew * self.ledger.position)
        limit = self.settings.account.max_position
        # The exchange counts open orders as if they filled. The cancels just sent reach it before
        # these quotes, so only orders that stay open count.
        staying = [order for order in self.ledger.orders.values() if not order.cancel_requested]
        buying = sum(order.leaves for order in staying if order.side == "buy")
        selling = sum(order.leaves for order in staying if order.side == "sell")
        # Only prices the exchange takes: 1 to 99 in a prediction market.
        bid, ask = center - self.half_spread, center + self.half_spread
        highest = self.settings.max_price
        if 1 <= bid < highest and self.ledger.position + buying + self.size <= limit:
            self.buy(bid, self.size, "post-only")
        if 1 < ask <= highest and self.ledger.position - selling - self.size >= -limit:
            self.sell(ask, self.size, "post-only")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7878)
    parser.add_argument("--seat", default="you")
    parser.add_argument("--token")
    args = parser.parse_args()
    end = crowdbook.run(MarketMaker(), args.host, args.port, args.seat, args.token)
    print(f"market maker done: {end}")


if __name__ == "__main__":
    main()
