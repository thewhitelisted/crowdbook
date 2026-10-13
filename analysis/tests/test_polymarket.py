import hashlib
import io
import unittest
import urllib.error
from unittest import mock

from crowdbook_analysis import polygon, polymarket

# A Bitcoin up-or-down market for 9:00 to 9:15 PM ET on October 11, 2026, as Polymarket lists it.
CONDITION = "0x6df5fa441e6375c8bc0ca9f326108b2824c6eff41d475e68da6faf978a91d594"
UP = 82084678012225664693063437504331247999494732864875520196394721117234751552284
DOWN = 27450529170832171099265703992872334869784040527210451914064977118372052916693


def word(value: int) -> str:
    return f"{value:064x}"


class KeccakTest(unittest.TestCase):
    def test_matches_known_digests(self):
        self.assertEqual(polygon.keccak256(b"").hex(),
                         "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470")
        self.assertEqual(polygon.event_topic("Transfer(address,address,uint256)"),
                         "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef")

        self.assertEqual(polygon.keccak256(b"The quick brown fox jumps over the lazy dog").hex(),
                         "4d741b6f1eb29cb2a9b9911c82f56fa8d73b04959d3d9d222895df6c0b28aa15")

    def test_the_same_sponge_gives_sha3_at_every_length(self):
        # SHA3-256 differs from Keccak-256 only in its padding, so checking the sponge against
        # hashlib's covers inputs that fill one block of 136 bytes exactly, or several.
        for length in sorted({*range(0, 420, 7), 135, 136, 271, 272}):
            data = bytes(range(256)) * 2
            self.assertEqual(polygon._sponge(data[:length], 0x06),
                             hashlib.sha3_256(data[:length]).digest(), length)

    def test_names_the_events_read(self):
        self.assertEqual(polymarket.ORDER_FILLED,
                         "0xd543adfd945773f1a62f74f0ee55a5e3b9b1a28262980ba90b1a89f2ea84d8ee")
        self.assertEqual(polymarket.CONDITION_RESOLUTION,
                         "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894")


class ShareIdTest(unittest.TestCase):
    def test_matches_the_ids_polymarket_lists(self):
        self.assertEqual(polymarket.share_id(CONDITION, 0), UP)
        self.assertEqual(polymarket.share_id(CONDITION, 1), DOWN)


class DecodeTest(unittest.TestCase):
    def test_reads_an_aggressive_sell(self):
        # A taker selling 121.71 Up shares for $120.4929 at 99 cents, with a fee of $0.08434.
        log = {
            "blockNumber": hex(95374949), "logIndex": "0x7", "blockTimestamp": hex(1791767702),
            "topics": [polymarket.ORDER_FILLED,
                       "0xc148687671b8ddb21517f5929313a2c3014be1abfb38a532063f76a45feaef5f",
                       polygon.address_topic("0x4aaf3b7df0d9ce28243a29fb668500aa791ee80c"),
                       polygon.address_topic(polymarket.EXCHANGE)],
            "data": "0x" + "".join(word(v) for v in
                                   (1, UP, 121_710_000, 120_492_900, 84_340, 0, 0)),
        }
        fill = polymarket.taker_fill(log)
        self.assertEqual(fill["token"], UP)
        self.assertFalse(fill["buy"])
        self.assertAlmostEqual(fill["shares"], 121.71)
        self.assertAlmostEqual(fill["price"], 99.0)
        self.assertAlmostEqual(fill["fee"], 0.08434)
        self.assertEqual(fill["trader"], "0x4aaf3b7df0d9ce28243a29fb668500aa791ee80c")
        self.assertEqual((fill["block"], fill["index"], fill["time"]), (95374949, 7, 1791767702))

    def test_reads_a_buy_as_dollars_for_shares(self):
        log = {"blockNumber": "0x1", "logIndex": "0x0", "blockTimestamp": "0x2",
               "topics": [polymarket.ORDER_FILLED, "0x0", polygon.address_topic("0x1"),
                          polygon.address_topic(polymarket.EXCHANGE)],
               "data": "0x" + "".join(word(v) for v in (0, DOWN, 3_000_000, 10_000_000, 0, 0, 0))}
        fill = polymarket.taker_fill(log)
        self.assertTrue(fill["buy"])
        self.assertAlmostEqual(fill["shares"], 10.0)
        self.assertAlmostEqual(fill["price"], 30.0)

    def test_reads_which_outcome_won(self):
        log = {"blockTimestamp": hex(1791767787),
               "topics": [polymarket.CONDITION_RESOLUTION, CONDITION, "0x0", "0x0"],
               "data": "0x" + "".join(word(v) for v in (2, 0x40, 2, 1, 0))}
        settled = polymarket.resolution(log)
        self.assertEqual(settled["condition"], CONDITION)
        self.assertEqual(settled["yes_payout"], 1.0)
        self.assertEqual(settled["time"], 1791767787)


class FakeNode(polygon.Rpc):
    """A node with one log per block that refuses requests covering more than `most` blocks."""

    def __init__(self, most: int):
        super().__init__("http://unused")
        self.most = most
        self.requests = 0

    def call(self, method, params):
        assert method == "eth_getLogs"
        self.requests += 1
        first, last = int(params[0]["fromBlock"], 16), int(params[0]["toBlock"], 16)
        if last - first + 1 > self.most:
            raise polygon.RpcError("Query returned more than 20000 results")
        return [{"blockNumber": hex(block)} for block in range(first, last + 1)]


class LogsTest(unittest.TestCase):
    def test_splits_ranges_the_node_refuses_and_misses_nothing(self):
        node = FakeNode(most=300)
        blocks = [int(log["blockNumber"], 16) for log in node.logs("0x0", [], 10, 2_009)]
        self.assertEqual(blocks, list(range(10, 2_010)))

    def test_passes_on_other_errors(self):
        class Broken(polygon.Rpc):
            def call(self, method, params):
                raise polygon.RpcError("execution reverted")

        with self.assertRaises(polygon.RpcError):
            list(Broken("http://unused").logs("0x0", [], 0, 10))


class RefusalTest(unittest.TestCase):
    def test_a_json_rpc_error_sent_as_http_400_is_the_nodes_answer(self):
        body = b'{"jsonrpc":"2.0","id":1,"error":{"code":35,"message":"ranges over 10000 blocks"}}'
        refusal = urllib.error.HTTPError("http://node", 400, "Bad Request", {}, io.BytesIO(body))
        with mock.patch("urllib.request.urlopen", side_effect=refusal):
            with self.assertRaisesRegex(polygon.RpcError, "ranges over"):
                polygon.Rpc("http://node").call("eth_getLogs", [])

    def test_an_http_400_without_one_is_a_failure(self):
        refusal = urllib.error.HTTPError("http://node", 400, "Bad Request", {},
                                         io.BytesIO(b"<html>"))
        with mock.patch("urllib.request.urlopen", side_effect=refusal):
            with self.assertRaises(urllib.error.HTTPError):
                polygon.Rpc("http://node").call("eth_getLogs", [])


if __name__ == "__main__":
    unittest.main()
