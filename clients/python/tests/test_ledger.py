import unittest

from crowdbook import Book, Ledger
from crowdbook.protocol import (Accepted, Cancel, Cancelled, Depth, Filled, Level, Modified,
                                NewOrder, Rejected, Top, Trade)


def bid(order_id, price, quantity):
    return NewOrder(id=order_id, side="buy", order_type="limit", price=price, quantity=quantity)


class LedgerTest(unittest.TestCase):
    def test_an_order_is_in_flight_until_acknowledged(self):
        ledger = Ledger(cash=100)
        ledger.record(bid(1, 99, 4))
        self.assertFalse(ledger.orders[1].acknowledged)
        self.assertEqual(ledger.open_quantity("buy"), 4)
        ledger.apply(Accepted(time=1, id=1, order_id=7, side="buy", order_type="limit",
                              quantity=4, price=99))
        self.assertTrue(ledger.orders[1].acknowledged)
        self.assertEqual(ledger.orders[1].order_id, 7)

    def test_fills_move_cash_position_and_fees(self):
        ledger = Ledger(cash=100, position=1)
        ledger.record(bid(1, 99, 4))
        ledger.apply(Filled(time=2, id=1, order_id=7, side="buy", price=99, quantity=3, leaves=1,
                            liquidity="maker", fee=-300))
        self.assertEqual((ledger.cash, ledger.position, ledger.fees), (100 - 297, 4, -300))
        self.assertEqual(ledger.orders[1].leaves, 1)
        ledger.apply(Filled(time=3, id=1, order_id=7, side="buy", price=99, quantity=1, leaves=0,
                            liquidity="maker", fee=-100))
        self.assertNotIn(1, ledger.orders)

    def test_a_cancel_that_crosses_a_fill(self):
        ledger = Ledger()
        ledger.record(NewOrder(id=1, side="sell", order_type="limit", price=101, quantity=2))
        ledger.record(Cancel(id=1))
        self.assertTrue(ledger.orders[1].cancel_requested)
        ledger.apply(Filled(time=2, id=1, order_id=7, side="sell", price=101, quantity=2,
                            leaves=0, liquidity="maker", fee=0))
        ledger.apply(Rejected(time=3, id=1, request="cancel", reason="unknown-order-id"))
        self.assertEqual((ledger.cash, ledger.position), (202, -2))
        self.assertEqual(ledger.orders, {})

    def test_rejected_new_orders_and_cancels_are_forgotten(self):
        ledger = Ledger()
        ledger.record(bid(1, 99, 4))
        ledger.apply(Rejected(time=1, id=1, request="new", reason="position-limit"))
        self.assertEqual(ledger.orders, {})
        ledger.record(bid(1, 99, 4))  # the id is free again
        ledger.apply(Modified(time=2, id=1, order_id=3, price=98, quantity=2))
        self.assertEqual((ledger.orders[1].price, ledger.orders[1].leaves), (98, 2))
        ledger.apply(Cancelled(time=3, id=1, order_id=3, quantity=2, reason="requested"))
        self.assertEqual(ledger.orders, {})

    def test_live_ids_cannot_be_reused_and_unknown_orders_are_errors(self):
        ledger = Ledger()
        ledger.record(bid(1, 99, 4))
        with self.assertRaises(ValueError):
            ledger.record(bid(1, 98, 1))
        with self.assertRaises(KeyError):
            ledger.apply(Cancelled(time=3, id=2, order_id=3, quantity=2, reason="requested"))


class BookTest(unittest.TestCase):
    def test_keeps_the_best_prices_the_depth_and_the_last_trade(self):
        book = Book()
        self.assertIsNone(book.mid)
        book.apply(Top(time=1, bid=Level(99, 5, 1), ask=Level(102, 1, 1)))
        book.apply(Depth(time=1, bids=(Level(99, 5, 1), Level(98, 2, 1)), asks=()))
        book.apply(Trade(time=2, price=100, quantity=3, aggressor="buy"))
        self.assertEqual(book.mid, 100.5)
        self.assertEqual(len(book.bids), 2)
        self.assertEqual((book.last_price, book.trades, book.volume), (100, 1, 3))


if __name__ == "__main__":
    unittest.main()
