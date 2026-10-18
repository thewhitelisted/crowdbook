import tomllib
import unittest

import numpy as np

from crowdbook_analysis import learner

STATE = ["time", "time_left", "position", "cash", "fees", "open_buy", "open_sell", "bid_price",
         "bid_quantity", "ask_price", "ask_quantity", "last_trade", "phase", "reference_price",
         "max_price", "max_position", "max_order_quantity", "rejections"]
SECOND = 1_000_000_000


def observation(rows: list[dict], events: list[list[list[int]]] | None = None) -> dict:
    """An observation of len(rows) markets, each row's state named, with its events."""
    state = np.zeros((len(rows), len(STATE)), dtype=np.int64)
    for market, row in enumerate(rows):
        for name, value in row.items():
            state[market, STATE.index(name)] = value
    width = max((len(e) for e in events or []), default=0) or 1
    stream = np.zeros((len(rows), width, 7), dtype=np.int64)
    counts = np.zeros((len(rows), 3), dtype=np.int64)
    for market, kept in enumerate(events or []):
        for k, event in enumerate(kept):
            stream[market, k, :len(event)] = event
        counts[market, 0] = len(kept)
    return {"state": state, "events": stream, "counts": counts}


def trade(time: int, price: int, quantity: int, aggressor: int) -> list[int]:
    return [1, time, price, quantity, aggressor, 0, 0]


def book(time: int, position: int = 0, bid: int = 99, ask: int = 101, sizes=(10, 10),
         last: int = 0) -> dict:
    return {"time": time, "position": position, "bid_price": bid, "ask_price": ask,
            "bid_quantity": sizes[0], "ask_quantity": sizes[1], "last_trade": last,
            "max_position": 10}


class SeatTest(unittest.TestCase):
    def test_replaces_the_scenarios_seat_and_learning_section(self):
        text = ('seed = 1\n[participant]\nlatency = { to_exchange = "50ms" }\n'
                '[learning]\ninterval = "5s"\nfeed = { lag = "1s" }\n'
                '[learning.ranges]\nduration = ["1s", "2s"]\n'
                '[[agents]]\ntype = "momentum"\n'
                '[ "exchange" ]\ntaker_fee = 0.2  # kept\n')
        seated = tomllib.loads(learner.seat(text, latency="2ms", size=3, max_position=7,
                                            interval="1s"))
        self.assertEqual(seated["participant"]["latency"]["to_exchange"], "2ms")
        self.assertEqual(seated["participant"]["account"],
                         {"max_position": 7, "max_order_quantity": 3})
        self.assertEqual(seated["learning"]["interval"], "1s")
        self.assertEqual(seated["learning"]["sizes"], [3])
        self.assertNotIn("feed", seated["learning"])  # never the value
        self.assertNotIn("ranges", seated["learning"])
        self.assertEqual(seated["agents"], [{"type": "momentum"}])
        self.assertEqual(seated["exchange"]["taker_fee"], 0.2)
        self.assertEqual(seated["seed"], 1)

    def test_a_scenario_without_a_seat_gets_one(self):
        seated = tomllib.loads(learner.seat('seed = 2\n[[agents]]\ntype = "momentum"\n',
                                            latency="1ms", size=1, max_position=10,
                                            interval="250ms"))
        self.assertEqual(seated["learning"]["warmup"], "250ms")
        self.assertEqual(len(seated["agents"]), 1)

    def test_valued_as_the_probes_are(self):
        # At the true value where there is one, at the last trade otherwise; never closed
        # through the book.
        for text, mark in (('[fundamental]\nvolatility = 1.0\n', "value"),
                           ('[prediction]\nprobability = 0.5\n', "value"), ("seed = 1\n", "last")):
            seated = tomllib.loads(learner.seat(text, latency="1ms", size=1, max_position=10,
                                                interval="1s"))
            self.assertEqual(seated["learning"]["close"], mark)
            self.assertEqual(seated["learning"]["reward_mark"], mark)

    def test_quoted_headers_end_a_table_left_out_and_are_left_out_themselves(self):
        text = ('[participant]\nlatency = { to_exchange = "50ms" }\n'
                '["exchange"]\ntaker_fee = 0.2\n'
                '[ "learning" ]\ninterval = "5s"\n'
                "['agents'.x]\ny = 1\n")
        seated = tomllib.loads(learner.seat(text, latency="1ms", size=1, max_position=10,
                                            interval="1s"))
        self.assertEqual(seated["exchange"]["taker_fee"], 0.2)
        self.assertEqual(seated["learning"]["interval"], "1s")
        self.assertEqual(seated["agents"]["x"]["y"], 1)

    def test_durations(self):
        self.assertEqual(learner._nanoseconds("1s"), SECOND)
        self.assertEqual(learner._nanoseconds("250ms"), 250_000_000)
        self.assertEqual(learner._nanoseconds("2m"), 120 * SECOND)
        with self.assertRaises(ValueError):
            learner._nanoseconds("soon")


class FeaturesTest(unittest.TestCase):
    def features(self) -> learner.Features:
        return learner.Features(2, {"state": STATE})

    def test_flow_is_the_signed_share_of_volume_fading_by_its_half_lives(self):
        f = self.features()
        x = f.update(observation([book(SECOND), book(SECOND)],
                                 [[trade(1, 101, 3, 1), trade(1, 99, 1, -1)], []]))
        flow = slice(0, len(learner.HALF_LIVES))
        np.testing.assert_allclose(x[0, flow], 0.5)  # (3 - 1) / 4
        np.testing.assert_allclose(x[1, flow], 0.0)
        # Two seconds on, a sell of 4: the 2 s memory keeps half of what it had.
        x = f.update(observation([book(3 * SECOND), book(3 * SECOND)],
                                 [[trade(2, 99, 4, -1)], []]))
        self.assertAlmostEqual(x[0, 0], (2 * 0.5 - 4) / (4 * 0.5 + 4))
        self.assertLess(x[0, 0], x[0, 1])  # the longer memories keep more of the buying
        self.assertEqual(x[0, -1], 1.0)

    def test_trend_touch_spread_position_and_last_trade(self):
        f = self.features()
        f.update(observation([book(SECOND), book(SECOND)]))
        x = f.update(observation([book(2 * SECOND, position=-5, bid=103, ask=105,
                                       sizes=(30, 10), last=105),
                                  book(2 * SECOND, bid=0, ask=105)]))
        k = len(learner.HALF_LIVES)
        names = list(learner.FEATURES)
        self.assertGreater(x[0, k], 0)  # the mid rose 4 ticks: up on every horizon
        self.assertLess(x[0, k], x[0, k + 1])  # the shortest memory has caught up most
        self.assertAlmostEqual(x[0, names.index("touch imbalance")], 0.5)
        self.assertAlmostEqual(x[0, names.index("spread")], np.tanh(1 / 4))
        self.assertAlmostEqual(x[0, names.index("position")], -0.5)
        self.assertAlmostEqual(x[0, names.index("last trade")], np.tanh(0.5))
        # With one side empty the mid is the last one seen and the spread reads as wide.
        self.assertEqual(x[1, names.index("spread")], 1.0)
        self.assertEqual(x[1, names.index("touch imbalance")], 0.0)

    def test_a_reset_market_starts_afresh(self):
        f = self.features()
        f.update(observation([book(SECOND), book(SECOND)],
                             [[trade(1, 101, 3, 1)], [trade(1, 101, 3, 1)]]))
        f.reset(np.array([True, False]))
        x = f.update(observation([book(SECOND), book(2 * SECOND)]))
        self.assertEqual(x[0, 0], 0.0)
        self.assertGreater(x[1, 0], 0.0)


class OrdersTest(unittest.TestCase):
    def weights(self, target: float, bid: float, ask: float, markets: int = 1) -> np.ndarray:
        w = np.zeros((markets, len(learner.HEADS), len(learner.FEATURES)))
        w[:, :, -1] = [target, bid, ask]  # on the bias, which is always 1
        return w

    def orders(self, weights, rows):
        x = np.zeros((len(rows), len(learner.FEATURES)))
        x[:, -1] = 1.0
        state = observation(rows)["state"]
        return learner.orders(weights, x, state, STATE, size=2, max_position=10)

    def test_takes_toward_its_limit_past_a_score_of_one_and_holds_between(self):
        self.assertEqual(list(self.orders(self.weights(1.5, 0, 0), [book(0)])[0]),
                         [0, 0, 0, 0, 2])
        self.assertEqual(list(self.orders(self.weights(-1.5, 0, 0), [book(0, position=-9)])[0]),
                         [0, 0, 0, 0, -1])
        self.assertEqual(self.orders(self.weights(0.9, 0, 0), [book(0, position=4)])[0, 4], 0)
        # Never toward a side with nothing on it.
        self.assertEqual(self.orders(self.weights(2.0, 0, 0), [book(0, ask=0)])[0, 4], 0)

    def test_rests_at_the_best_prices_where_their_scores_are_above_zero(self):
        self.assertEqual(list(self.orders(self.weights(0, 0.1, 0.1), [book(0)])[0]),
                         [99, 2, 101, 2, 0])
        self.assertEqual(list(self.orders(self.weights(0, 0.1, -0.1), [book(0)])[0]),
                         [99, 2, 0, 0, 0])
        # A take cancels the quote on the side it takes from, a buy the offer, so none is asked
        # for there.
        self.assertEqual(list(self.orders(self.weights(2.0, 0.1, 0.1), [book(0)])[0]),
                         [99, 2, 0, 0, 2])
        self.assertEqual(list(self.orders(self.weights(-2.0, 0.1, 0.1), [book(0)])[0]),
                         [0, 0, 101, 2, -2])


class FakePool:
    """A stand-in for crowdbook_learn's pool: each decision a second apart, a trade whose side
    says which way the value moves before the next, the side changing at random, by the seed, a
    tenth of the time; a position earns `pays` a lot for each tick the value moves its way, and
    an episode lasts `steps` decisions. Every third decision of a market is a fill's, off the
    schedule."""

    pays = 1.0
    steps = 30
    made = []  # every pool's takes

    sent = []  # every pool's orders
    resting = False  # whether the quotes it is sent rest, unfilled
    stop_at = None  # the decision at which the loss limit stops the seat, if any

    def __init__(self, markets: int):
        self.markets = markets
        self.observation = observation([book(SECOND)] * markets, [[trade(0, 100, 1, 1)]] *
                                       markets)
        self.terminated = np.zeros(markets, dtype=bool)
        self.position = np.zeros(markets)
        self.step_count = np.zeros(markets, dtype=int)
        self.side = np.zeros(markets)
        self.rng = [np.random.default_rng(0)] * markets
        self.time = np.zeros(markets, dtype=np.int64)
        self.scenarios = [None] * markets
        self.history = []  # the scenarios after each reset
        self.stopped = np.zeros(markets, dtype=bool)

    @classmethod
    def from_text(cls, text: str, markets: int, threads: int = 0):
        return cls(markets)

    def _show(self, market: int) -> None:
        if self.side[market] == 0 or self.rng[market].random() < 0.1:
            self.side[market] = 1 if self.rng[market].random() < 0.5 else -1
        state = self.observation["state"]
        state[market, STATE.index("time")] = self.time[market]
        state[market, STATE.index("position")] = self.position[market]
        state[market, STATE.index("bid_price")] = 99
        state[market, STATE.index("ask_price")] = 101
        state[market, STATE.index("max_position")] = 10
        sent = FakePool.sent[-1][market] if FakePool.sent and FakePool.resting else np.zeros(5)
        state[market, STATE.index("open_buy")] = sent[1]
        state[market, STATE.index("open_sell")] = sent[3]
        self.observation["events"][market, 0] = trade(0, 100, 1, int(self.side[market]))
        self.observation["counts"][market, 0] = 1

    def reset(self, seeds, mask, scenarios):
        for market in np.flatnonzero(mask):
            self.rng[market] = np.random.default_rng(seeds[market])
            self.position[market] = 0
            self.side[market] = 0
            self.step_count[market] = 0
            self.time[market] = SECOND
            self.terminated[market] = False
            self.scenarios[market] = scenarios[market]
            self._show(market)
        self.history.append(list(self.scenarios))

    def step_orders(self, orders):
        FakePool.made.append(orders[:, 4].copy())
        FakePool.sent.append(orders.copy())
        rewards = np.zeros(self.markets)
        for market in range(self.markets):
            if self.terminated[market]:
                raise RuntimeError("stepped after its end")
            self.position[market] = np.clip(self.position[market] + orders[market, 4], -10, 10)
            rewards[market] = self.pays * self.position[market] * self.side[market]
            self.step_count[market] += 1
            # Every third decision is a fill's, a third of a second on; the others on the second.
            off = self.step_count[market] % 3 == 1
            self.time[market] = (self.time[market] // SECOND + 1) * SECOND - (
                SECOND // 3 if off else 0)
            self.terminated[market] = self.step_count[market] >= self.steps
            stopped = self.stop_at is not None and self.step_count[market] == self.stop_at
            if stopped:
                rewards[market] = -1000.0  # the loss that crossed the limit, and its close
            self.stopped[market] = stopped
            self.terminated[market] |= stopped
            self._show(market)
        return self.observation, rewards, self.terminated, None, {"stopped": self.stopped}


class FakeLearn:
    Pool = FakePool
    STATE_NAMES = STATE


class TrainingTest(unittest.TestCase):
    def setUp(self):
        FakePool.pays, FakePool.made, FakePool.sent, FakePool.resting = 1.0, [], [], False
        FakePool.stop_at = None

    def settings(self, **changes) -> learner.Settings:
        return learner.Settings(**{"generations": 8, "markets": 8, "size": 5, **changes})

    def test_plays_every_job_once_refilling_the_pool(self):
        weights = np.zeros((5, len(learner.HEADS), len(learner.FEATURES)))
        weights[:, 0, 0] = 5.0  # follows the last trade's side, all in
        played = learner.play(FakeLearn, ["a", "b"], weights, [1, 2, 3, 1, 2],
                              self.settings(), pool_markets=2)
        self.assertEqual(len(played.made), 5)
        # A seed's market is the same whichever pool market runs it.
        self.assertEqual(played.made[0], played.made[3])
        self.assertEqual(played.made[1], played.made[4])
        self.assertGreater(played.made.min(), 0)
        self.assertTrue((np.concatenate(FakePool.made) != 0).any())

    def test_takes_nothing_at_a_decision_off_its_schedule(self):
        weights = np.zeros((1, len(learner.HEADS), len(learner.FEATURES)))
        weights[0, 0, -1] = 5.0  # buys whenever it may
        FakePool.made = []
        learner.play(FakeLearn, ["a"], weights, [1], self.settings())
        takes = [int(t[0]) for t in FakePool.made]
        # Decisions 1, 4, 7, ... come a third of a second early, a fill's.
        for k, take in enumerate(takes):
            if k % 3 == 1:
                self.assertEqual(take, 0, k)
        self.assertGreater(sum(takes), 0)

    def test_between_decisions_it_keeps_only_the_quotes_it_still_has(self):
        weights = np.zeros((1, len(learner.HEADS), len(learner.FEATURES)))
        weights[0, 1:, -1] = 1.0  # quotes both sides at every decision of its own
        for resting in (False, True):
            FakePool.sent, FakePool.resting = [], resting
            learner.play(FakeLearn, ["a"], weights, [1], self.settings())
            for k, orders in enumerate(FakePool.sent):
                quoted = list(orders[0, [1, 3]])
                # Decisions 1, 4, 7, ... are a fill's: a quote filled there is not placed again.
                self.assertEqual(quoted, [5, 5] if k % 3 != 1 or resting else [0, 0], k)

    def test_trains_on_all_but_a_settlement_and_never_leaves_out_a_stop(self):
        weights = np.zeros((2, len(learner.HEADS), len(learner.FEATURES)))
        weights[:, 0, 0] = 5.0
        whole = learner.play(FakeLearn, ["a"], weights, [1, 2], self.settings())
        self.assertTrue((whole.marked != whole.made).all())  # the last step is the close
        FakePool.stop_at = 10
        stopped = learner.play(FakeLearn, ["a"], weights, [1, 2], self.settings())
        np.testing.assert_array_equal(stopped.marked, stopped.made)
        self.assertTrue((stopped.made < -900).all())

    def test_a_seed_runs_the_same_scenario_for_every_candidate(self):
        pools = []
        real = FakePool.from_text
        FakePool.from_text = classmethod(lambda cls, text, markets, threads=0:
                                         pools.append(cls(markets)) or pools[-1])
        try:
            weights = np.zeros((6, len(learner.HEADS), len(learner.FEATURES)))
            learner.play(FakeLearn, ["a", "b", "c"], weights, [7, 8, 7, 8, 7, 8], self.settings())
        finally:
            FakePool.from_text = real
        self.assertEqual(pools[0].history[0], ["b", "c", "b", "c", "b", "c"])

    def test_finds_an_edge_planted_in_the_flow_and_none_where_there_is_none(self):
        # Known answer: where the last trade's side says where the value goes, the learner
        # learns to follow it and is flagged on fresh markets; where a position earns nothing,
        # it is not.
        weights, history = learner.train(FakeLearn, ["a"], self.settings(), log=lambda _: None)
        judged = learner.summary(learner.judge(FakeLearn, ["a"], weights, 64, self.settings()))
        self.assertGreater(judged["low"], 0)
        self.assertGreater(weights[0, 0], 0)  # on the 2 s flow
        self.assertEqual(len(history), 8)
        FakePool.pays = 0.0
        weights, _ = learner.train(FakeLearn, ["a"], self.settings(), log=lambda _: None)
        judged = learner.summary(learner.judge(FakeLearn, ["a"], weights, 64, self.settings()))
        self.assertFalse(judged["low"] > 0)

    def test_the_weights_table_names_every_feature(self):
        table = learner.weights_table(np.zeros((len(learner.HEADS), len(learner.FEATURES))))
        for name in learner.FEATURES:
            self.assertIn(f"| {name} |", table)


if __name__ == "__main__":
    unittest.main()
