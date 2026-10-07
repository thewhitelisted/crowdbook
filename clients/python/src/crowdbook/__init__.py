"""A client for crowdbook: trade in a served market from Python.

    import crowdbook

    class Buyer(crowdbook.Bot):
        def on_start(self):
            self.buy_market(5)

    crowdbook.run(Buyer(), port=7878, seat="alice")
"""

from .client import Book, Bot, Connection, Ledger, Order, run
from .protocol import ProtocolError

__all__ = ["Book", "Bot", "Connection", "Ledger", "Order", "ProtocolError", "run"]
