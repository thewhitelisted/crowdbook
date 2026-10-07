import unittest

from crowdbook import protocol
from crowdbook.protocol import (Accepted, Cancel, Depth, End, Error, Filled, Hello, Level, Modify,
                                NewOrder, ProtocolError, Rejected, Start, Top, Trade, Welcome)


class EncodeTest(unittest.TestCase):
    def test_messages_are_encoded_as_the_specification_shows(self):
        self.assertEqual(protocol.encode(Hello(seat="alice", token="s3cret")),
                         '{"type":"hello","protocol":1,"seat":"alice","token":"s3cret"}\n')
        self.assertEqual(
            protocol.encode(NewOrder(id=17, side="buy", order_type="limit", price=10003,
                                     quantity=5)),
            '{"type":"new","id":17,"side":"buy","order_type":"limit",'
            '"time_in_force":"good-till-cancel","price":10003,"quantity":5}\n')
        self.assertEqual(
            protocol.encode(NewOrder(id=18, side="sell", order_type="market", quantity=2,
                                     parent=7)),
            '{"type":"new","id":18,"side":"sell","order_type":"market","quantity":2,'
            '"parent":7}\n')
        self.assertEqual(protocol.encode(Cancel(id=17)), '{"type":"cancel","id":17}\n')
        self.assertEqual(protocol.encode(Modify(id=17, price=10004, quantity=3)),
                         '{"type":"modify","id":17,"price":10004,"quantity":3}\n')

    def test_orders_that_make_no_sense_are_not_sent(self):
        for order in (NewOrder(id=1, side="buy", order_type="limit", quantity=1),
                      NewOrder(id=1, side="buy", order_type="market", quantity=1, price=5),
                      NewOrder(id=1, side="long", order_type="market", quantity=1),  # type: ignore
                      NewOrder(id=1, side="buy", order_type="limit", quantity=1, price=5,
                               time_in_force="forever")):  # type: ignore
            with self.assertRaises(ValueError):
                protocol.encode(order)


class DecodeTest(unittest.TestCase):
    def test_decodes_every_server_message(self):
        welcome = protocol.decode(
            '{"type":"welcome","protocol":1,"seat":"alice","started":false,"time":0,'
            '"duration":60000000000,"reference_price":10000,"depth_levels":10,"maker_fee":-100,'
            '"taker_fee":200,"latency":{"to_exchange":1000000,"from_exchange":1000000,'
            '"jitter":0},"account":{"initial_cash":0,"initial_position":0,"cash":5,'
            '"position":2,"fees":0,"max_position":50,"max_order_quantity":10},'
            '"orders":[{"id":4,"order_id":9,"side":"sell","order_type":"limit","price":10010,'
            '"leaves":3,"acknowledged":true,"cancel_requested":false}]}\n')
        self.assertIsInstance(welcome, Welcome)
        self.assertEqual(welcome.account.position, 2)
        self.assertEqual(welcome.latency.from_exchange, 1_000_000)
        self.assertEqual(welcome.orders[0].price, 10010)
        self.assertEqual(protocol.decode('{"type":"start","time":0}'), Start(time=0))
        self.assertEqual(
            protocol.decode('{"type":"accepted","time":1,"id":17,"order_id":4211,"side":"buy",'
                            '"order_type":"market","quantity":5}'),
            Accepted(time=1, id=17, order_id=4211, side="buy", order_type="market", quantity=5))
        self.assertEqual(
            protocol.decode('{"type":"rejected","time":1,"id":17,"request":"new",'
                            '"reason":"position-limit"}'),
            Rejected(time=1, id=17, request="new", reason="position-limit"))
        self.assertEqual(
            protocol.decode('{"type":"filled","time":3,"id":17,"order_id":4211,"side":"buy",'
                            '"price":10004,"quantity":2,"leaves":1,"liquidity":"maker",'
                            '"fee":-200}').fee, -200)
        self.assertEqual(
            protocol.decode('{"type":"trade","time":3,"price":10004,"quantity":2,'
                            '"aggressor":"sell"}'),
            Trade(time=3, price=10004, quantity=2, aggressor="sell"))
        self.assertEqual(
            protocol.decode('{"type":"top","time":3,"bid":{"price":10003,"quantity":12,'
                            '"orders":3},"ask":null}'),
            Top(time=3, bid=Level(price=10003, quantity=12, orders=3), ask=None))
        self.assertEqual(
            protocol.decode('{"type":"depth","time":3,"bids":[{"price":10003,"quantity":12,'
                            '"orders":3}],"asks":[]}'),
            Depth(time=3, bids=(Level(10003, 12, 3),), asks=()))
        self.assertEqual(
            protocol.decode('{"type":"end","time":9,"cash":-50012,"position":5,"fees":1000,'
                            '"pnl":45}'),
            End(time=9, cash=-50012, position=5, fees=1000, pnl=45))
        self.assertEqual(protocol.decode('{"type":"error","message":"no","fatal":true}'),
                         Error(message="no", fatal=True))

    def test_challenges_scoring_and_scores(self):
        welcome = protocol.decode(
            '{"type":"welcome","protocol":1,"seat":"carol","started":true,"time":5,'
            '"duration":60,"reference_price":100,"depth_levels":0,"maker_fee":0,"taker_fee":0,'
            '"latency":{"to_exchange":0,"from_exchange":0,"jitter":0},"account":{'
            '"initial_cash":0,"initial_position":0,"cash":0,"position":0,"fees":0,'
            '"max_position":9,"max_order_quantity":9},"orders":[],'
            '"challenge":{"name":"Work a large order","briefing":"Buy 300."},'
            '"scoring":{"mark":"value","inventory_penalty":0,"close_penalty":0,"max_loss":0,'
            '"target":{"side":"buy","quantity":300,"benchmark":"vwap",'
            '"unfinished_penalty":5000}}}')
        self.assertEqual(welcome.challenge.name, "Work a large order")
        self.assertEqual(welcome.scoring.target.quantity, 300)
        end = protocol.decode(
            '{"type":"end","time":9,"cash":0,"position":0,"fees":0,"pnl":0,"score":{'
            '"total":-11512,"pnl":-3500,"inventory":12,"close":2000,"paper":-9000,'
            '"unfinished":15000,"unfinished_lots":3,"stopped_at":77}}')
        self.assertEqual(end.score.total, -11512)
        self.assertEqual(end.score.stopped_at, 77)
        plain = protocol.decode('{"type":"end","time":9,"cash":0,"position":0,"fees":0,'
                                '"pnl":0,"score":null}')
        self.assertIsNone(plain.score)

    def test_fields_a_newer_server_adds_are_ignored(self):
        self.assertEqual(protocol.decode('{"type":"clock","time":5,"sequence":12}').time, 5)

    def test_lines_that_are_not_messages_are_errors(self):
        for line in ("not json", "[]", '{"time":1}', '{"type":"ping"}',
                     '{"type":"trade","time":1}', '{"type":"top","time":1,"bid":{"price":1}}'):
            with self.assertRaises(ProtocolError, msg=line):
                protocol.decode(line)

    def test_fills_carry_their_fee(self):
        filled = protocol.decode(
            '{"type":"filled","time":3,"id":1,"order_id":2,"side":"sell","price":5,"quantity":1,'
            '"leaves":0,"liquidity":"taker","fee":300}')
        self.assertIsInstance(filled, Filled)
        self.assertEqual(filled.liquidity, "taker")


if __name__ == "__main__":
    unittest.main()
