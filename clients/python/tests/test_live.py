"""Bots trading against a real crowdbook server. Set CROWDBOOK to the crowdbook command to run
them; CTest does."""

import contextlib
import importlib.util
import json
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
CHALLENGES = pathlib.Path(__file__).resolve().parents[3] / "examples" / "challenges"
MILLISECOND = 1_000_000

LIMITS = """
duration = "10s"
reference_price = 1000

[exchange]
maker_fee = -0.1
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "2ms" }
account = { initial_position = 4, max_position = 5, max_order_quantity = 3 }

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
    yields the port. `scenario` is a path, or the text of a scenario. On the way out it waits for
    the server to finish."""
    if isinstance(scenario, str):
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
    """Tries the seat's limits once the book has filled in, and keeps every order event."""

    def __init__(self):
        super().__init__()
        self.sent = []  # the new orders' ids, in the order sent
        self.events = []
        self.tried_limits = False

    def on_start(self):
        self.wake_after(300 * MILLISECOND)

    def on_wakeup(self, tag):
        self.wake_after(100 * MILLISECOND)
        if self.book.bid is None or self.book.ask is None or self.ledger.orders:
            return
        if not self.tried_limits:
            self.tried_limits = True
            self.sent.append(self.buy_market(4))  # over the largest order of three
            self.sent.append(self.buy(900, 3))  # 4 held and 3 more would pass the limit of 5
        elif not any(isinstance(event, crowdbook.protocol.Filled) and event.liquidity == "taker"
                     for event in self.events) and self.ledger.position < 5:
            self.sent.append(self.buy_market(1))  # a taker, tried until it fills
        elif self.ledger.position > 0:
            self.sent.append(self.sell(self.book.ask.price, 1))  # a maker, if anyone takes it

    def record(self, event):
        self.events.append(event)

    on_accepted = on_rejected = on_filled = on_cancelled = on_modified = record


def example(name):
    """One of the example bots' modules."""
    spec = importlib.util.spec_from_file_location(name, EXAMPLES / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
        self.assertEqual(rejected[bot.sent[1]], "position-limit")

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
        bots = {"maker2": example("market_maker").MarketMaker(),
                "trend2": example("momentum").Momentum()}
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

    def test_the_example_bots_play_every_challenge(self):
        players = {"market_making": lambda: example("market_maker").MarketMaker(),
                   "large_order": lambda: example("twap").Twap(),
                   "news": lambda: example("momentum").Momentum(max_position=30),
                   "informed_flow": lambda: example("market_maker").MarketMaker()}
        self.assertEqual(sorted(players), sorted(path.stem for path in CHALLENGES.glob("*.toml")))
        for name, player in players.items():
            with self.subTest(challenge=name):
                directory = self.directory / name
                directory.mkdir()
                bot = player()
                with served(directory, CHALLENGES / f"{name}.toml", "--duration", "60s",
                            "--speed", "30") as port:
                    end = crowdbook.run(bot, port=port)
                self.assertTrue(bot.settings.challenge.name)
                self.assertIsNotNone(end.score)

                # The replay scores the session again, and comes to the same score.
                replay = subprocess.run(
                    [CROWDBOOK, "replay", str(directory / "session.toml"), "--json",
                     str(directory / "replay.json")], capture_output=True, text=True)
                self.assertEqual(replay.returncode, 0, replay.stderr)
                (again,) = json.loads((directory / "replay.json").read_text())["scores"]
                self.assertEqual(again["seat"], "you")
                self.assertEqual(
                    (again["total"], again["pnl"], again["inventory"], again["close"],
                     again["paper"], again["unfinished"], again["unfinished_lots"],
                     again["stopped_at_ns"]),
                    (end.score.total, end.score.pnl, end.score.inventory, end.score.close,
                     end.score.paper, end.score.unfinished, end.score.unfinished_lots,
                     end.score.stopped_at))

                # So does the session's report, which also knows who the bot traded with.
                report = subprocess.run(
                    [CROWDBOOK, "report", str(directory / "session.toml"), "--json",
                     str(directory / "report.json")], capture_output=True, text=True)
                self.assertEqual(report.returncode, 0, report.stderr)
                (seat,) = json.loads((directory / "report.json").read_text())["seats"]
                self.assertEqual(seat["score"]["total"], end.score.total)
                self.assertEqual(sum(party["bought"] + party["sold"]
                                     for party in seat["counterparties"]),
                                 sum(fill["quantity"] for fill in seat["fills"]))


if __name__ == "__main__":
    unittest.main()
