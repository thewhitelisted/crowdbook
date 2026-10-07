"""Bots trading against a real crowdbook server. Set CROWDBOOK to the crowdbook command to run
them; CTest does."""

import contextlib
import importlib.util
import os
import pathlib
import re
import subprocess
import tempfile
import threading
import tomllib
import unittest

import crowdbook

CROWDBOOK = os.environ.get("CROWDBOOK")
EXAMPLES = pathlib.Path(__file__).resolve().parent.parent / "examples"
SCENARIOS = pathlib.Path(__file__).resolve().parents[3] / "examples" / "scenarios"
MILLISECOND = 1_000_000

LIMITS = """
duration = "10s"
reference_price = 1000

[exchange]
maker_fee = -0.1
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "2ms" }
account = { max_position = 5, max_order_quantity = 3 }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 20
limit_rate = 5.0
market_rate = 1.0

[[agents]]
type = "market_maker"
name = "maker"
"""


@contextlib.contextmanager
def served(directory, scenario, *arguments):
    """Runs `crowdbook serve` on a free port, recording to session.toml in `directory`, and
    yields the port. On the way out it waits for the server to finish."""
    if not pathlib.Path(scenario).exists():
        (directory / "scenario.toml").write_text(scenario)
        scenario = directory / "scenario.toml"
    server = subprocess.Popen(
        [CROWDBOOK, "serve", str(scenario), "--listen", "127.0.0.1:0",
         "--record", str(directory / "session.toml"), *arguments],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        first = server.stdout.readline()
        found = re.search(r":(\d+) to seats", first)
        if not found:
            raise RuntimeError(f"the server did not start: {first}{server.stdout.read()}")
        yield int(found.group(1))
        output, _ = server.communicate(timeout=30)
        if server.returncode != 0:
            raise RuntimeError(f"the server failed: {output}")
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


class Tester(crowdbook.Bot):
    """Tries the seat's limits once the market opens, and keeps every order event."""

    def __init__(self):
        super().__init__()
        self.sent = []  # the new orders' ids, in the order sent
        self.events = []

    def on_start(self):
        self.sent.append(self.buy_market(4))  # over the largest order of three
        self.sent.append(self.buy_market(3))  # a taker
        self.sent.append(self.buy(900, 3))  # 3 held and 3 more would pass the limit of five
        self.wake_after(100 * MILLISECOND)

    def on_wakeup(self, tag):
        if not self.ledger.orders and self.ledger.position > 0:
            # Offer what was bought where buyers will take it, to be a maker.
            self.sent.append(self.sell((self.book.bid.price if self.book.bid else 1000) + 1, 3))
        else:
            self.wake_after(100 * MILLISECOND)

    def record(self, event):
        self.events.append(event)

    on_accepted = on_rejected = on_filled = on_cancelled = on_modified = record


@unittest.skipUnless(CROWDBOOK, "set CROWDBOOK to the crowdbook command to run against a server")
class LiveTest(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_a_bot_trades_under_the_same_limits_latency_and_fees(self):
        bot = Tester()
        directory = self.directory
        with served(directory, LIMITS, "--speed", "10") as port:
            end = crowdbook.run(bot, port=port)
        self.assertIsNotNone(end)
        settings = bot.settings

        rejected = {event.id: event.reason for event in bot.events
                    if isinstance(event, crowdbook.protocol.Rejected)}
        self.assertEqual(rejected[bot.sent[0]], "order-size-limit")
        self.assertEqual(rejected[bot.sent[2]], "position-limit")

        fills = [event for event in bot.events if isinstance(event, crowdbook.protocol.Filled)]
        self.assertTrue(any(fill.liquidity == "taker" for fill in fills))
        for fill in fills:
            rate = settings.taker_fee if fill.liquidity == "taker" else settings.maker_fee
            self.assertEqual(fill.fee, rate * fill.quantity)

        # Every answer from the exchange came one latency there and one back after the order
        # left, by the recording's clock.
        session = tomllib.loads((directory / "session.toml").read_text())
        left = [action["time_ns"] for action in session["actions"] if action["request"] == "new"]
        self.assertEqual(len(left), len(bot.sent))
        round_trip = settings.latency.to_exchange + settings.latency.from_exchange
        self.assertEqual(round_trip, 3 * MILLISECOND)
        for order_id, time in zip(bot.sent, left):
            answer = next(event for event in bot.events if event.id == order_id
                          and isinstance(event, (crowdbook.protocol.Accepted,
                                                 crowdbook.protocol.Rejected)))
            self.assertEqual(answer.time - time, round_trip)

        # The bot's own books agree with the exchange's.
        self.assertEqual((bot.ledger.position, bot.ledger.cash, bot.ledger.fees),
                         (end.position, end.cash, end.fees))

        replay = subprocess.run([CROWDBOOK, "replay", str(directory / "session.toml")],
                                capture_output=True, text=True)
        self.assertEqual(replay.returncode, 0, replay.stderr)

    def test_the_example_bots_trade_side_by_side(self):
        bots = {}
        for name, seat in (("market_maker", "maker2"), ("momentum", "trend2")):
            spec = importlib.util.spec_from_file_location(name, EXAMPLES / f"{name}.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            bots[seat] = module.MarketMaker() if name == "market_maker" else module.Momentum()
        ends = {}
        with served(self.directory, SCENARIOS / "playable.toml", "--seat", "maker2", "--seat",
                    "trend2", "--duration", "30s", "--speed", "15") as port:
            threads = [threading.Thread(
                target=lambda seat=seat: ends.__setitem__(seat, crowdbook.run(bots[seat],
                                                                              port=port,
                                                                              seat=seat)))
                for seat in bots]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=30)
        for seat, bot in bots.items():
            self.assertIsNotNone(ends.get(seat), seat)
            self.assertEqual((bot.ledger.position, bot.ledger.cash),
                             (ends[seat].position, ends[seat].cash), seat)
        self.assertGreater(bots["maker2"].book.trades, 0)


if __name__ == "__main__":
    unittest.main()
