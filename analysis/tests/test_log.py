import tempfile
import unittest
from pathlib import Path

from crowdbook_analysis.log import (
    aggressive_orders,
    executions,
    quotes,
    read_log,
    read_prices,
    trades,
)

HEADER = """time,kind,agent,client_order_id,order_id,side,type,time_in_force,price,quantity,leaves,\
liquidity,fee,request,reason,bid_price,bid_quantity,ask_price,ask_quantity
"""
LOG = HEADER + """100,new,1,7,,buy,limit,good-till-cancel,99,5,,,,,,,,,
100,top_of_book,,,,,,,,,,,,,,99,5,,
150,trade,,,,sell,,,99,2,,,,,,,,,
150,top_of_book,,,,,,,,,,,,,,99,3,101,4
"""


# Agent 3's buy sweeps two of agent 4's and 5's sell orders; then agent 4 sells one lot to agent 3.
# Each execution logs the maker's fill, then the taker's.
FILLS = HEADER + """200,filled,4,9,8,sell,,,101,2,0,maker,-0.2,,,,,,
200,filled,3,1,10,buy,,,101,2,3,taker,0.3,,,,,,
200,filled,5,2,7,sell,,,102,3,0,maker,-0.2,,,,,,
200,filled,3,1,10,buy,,,102,3,0,taker,0.3,,,,,,
300,filled,3,3,12,buy,,,100,1,0,maker,-0.2,,,,,,
300,filled,4,10,11,sell,,,100,1,0,taker,0.3,,,,,,
"""


class ReadLogTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.path = Path(self.scratch.name) / "log.csv"
        self.path.write_text(LOG)

    def tearDown(self):
        self.scratch.cleanup()

    def test_reads_every_row_with_empty_columns_as_null(self):
        log = read_log(self.path)
        self.assertEqual(log.height, 4)
        self.assertIsNone(log["order_id"][0])

    def test_quotes_carry_the_mid_and_spread_when_both_sides_are_quoted(self):
        book = quotes(read_log(self.path))
        self.assertEqual(book["time"].to_list(), [100, 150])
        self.assertEqual(book["mid"].to_list(), [None, 100.0])
        self.assertEqual(book["spread"].to_list(), [None, 2])

    def test_trades_keep_the_aggressor_side(self):
        prints = trades(read_log(self.path))
        self.assertEqual(prints.rows(), [(150, 99, 2, "sell")])


class FillsTest(unittest.TestCase):
    def setUp(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "log.csv"
            path.write_text(FILLS)
            self.log = read_log(path)

    def test_aggressive_orders_sum_each_taking_orders_fills(self):
        orders = aggressive_orders(self.log)
        self.assertEqual(orders.rows(), [(200, 3, 10, "buy", 5), (300, 4, 11, "sell", 1)])

    def test_reads_each_fills_fee(self):
        self.assertEqual(self.log["fee"].to_list(), [-0.2, 0.3, -0.2, 0.3, -0.2, 0.3])

    def test_executions_pair_each_maker_with_its_taker(self):
        self.assertEqual(
            executions(self.log).rows(),
            [(200, 101, 2, 4, "sell", 3), (200, 102, 3, 5, "sell", 3), (300, 100, 1, 3, "buy", 4)],
        )


class ReadPricesTest(unittest.TestCase):
    def test_adds_the_mid_where_both_sides_are_quoted(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "prices.csv"
            path.write_text("time,bid,ask,last_trade\n0,,,\n1000,99,101,100\n")
            prices = read_prices(path)
        self.assertEqual(prices["mid"].to_list(), [None, 100.0])


if __name__ == "__main__":
    unittest.main()
