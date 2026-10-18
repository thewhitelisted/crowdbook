"""The learning pool from Python: its arrays, its determinism, its lock and its errors. CTest runs
these with the built module on PYTHONPATH and CROWDBOOK_SOURCE_DIR set."""

import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import numpy as np

import crowdbook_learn

SOURCE = pathlib.Path(os.environ.get("CROWDBOOK_SOURCE_DIR",
                                     pathlib.Path(__file__).resolve().parents[2]))
MARKET = SOURCE / "examples" / "learning" / "market.toml"
CROWDBOOK = os.environ.get("CROWDBOOK")

try:
    import gymnasium
except ImportError:
    gymnasium = None


def actions_for(step: int, markets: int, actions: int, sizes: int) -> np.ndarray:
    """The same actions for the same step, whatever the pool."""
    rng = np.random.default_rng(step)
    return np.stack([rng.integers(0, actions, markets), rng.integers(0, sizes, markets)], axis=1)


def short(text: str) -> str:
    """The example market with episodes of 6 to 10 seconds."""
    return text.replace('duration = ["300s", "900s"]', 'duration = ["6s", "10s"]')


class PoolTest(unittest.TestCase):
    def test_arrays_view_the_pools_memory_and_have_its_shapes(self):
        pool = crowdbook_learn.Pool(MARKET, markets=3, threads=2)
        obs, info = pool.reset(seeds=1)
        config = pool.config
        self.assertEqual(obs["state"].shape, (3, len(crowdbook_learn.STATE_NAMES)))
        self.assertEqual(obs["events"].shape, (3, config["max_events"], 7))
        self.assertEqual(obs["depth"].shape,
                         (3, config["max_depth_updates"], 2, config["depth_levels"], 2))
        self.assertEqual(obs["book"].shape, (3, 2, config["depth_levels"], 2))
        self.assertEqual(obs["orders"].shape, (3, config["max_orders"], 5))
        self.assertEqual(obs["state"].dtype, np.int64)
        self.assertEqual(pool.rewards.dtype, np.float64)
        self.assertEqual(pool.terminated.dtype, np.bool_)
        self.assertEqual(info["settings"].shape, (3, len(pool.setting_names)))
        self.assertEqual(list(info["seeds"]), [1, 2, 3])
        self.assertFalse(obs["state"].flags.writeable)
        time = crowdbook_learn.STATE_NAMES.index("time")
        np.testing.assert_array_equal(obs["state"][:, time], 5_000_000_000)

        # The same arrays, at the same addresses, after every step, in new dicts each time.
        address = obs["events"].__array_interface__["data"][0]
        before = obs["state"].copy()
        returned = pool.step(np.zeros((3, 2), dtype=np.int64))
        self.assertIs(returned[0]["events"], obs["events"])
        self.assertIsNot(returned[0], obs)
        returned[4]["episode"] = "a wrapper's own"
        self.assertNotIn("episode", pool.info)
        self.assertEqual(obs["events"].__array_interface__["data"][0], address)
        np.testing.assert_array_equal(obs["state"][:, time], 6_000_000_000)
        self.assertFalse(np.array_equal(before, obs["state"]))
        self.assertEqual(pool.action_names[0], "hold")
        self.assertEqual(pool.sizes, [1, 5])

    def test_orders_at_named_prices_rest_where_asked_and_take(self):
        pool = crowdbook_learn.Pool(MARKET, markets=2, threads=1)
        obs, _ = pool.reset(seeds=1)
        names = crowdbook_learn.STATE_NAMES
        bid = obs["state"][:, names.index("bid_price")].copy()
        ask = obs["state"][:, names.index("ask_price")].copy()
        # Market 0 quotes two ticks outside the touch; market 1 buys two lots at the offer.
        orders = np.array([[bid[0] - 2, 3, ask[0] + 2, 4, 0], [0, 0, 0, 0, 2]], dtype=np.int64)
        obs, *_ = pool.step_orders(orders)
        open_orders = obs["orders"][0][obs["orders"][0][:, 2] > 0]
        self.assertEqual(sorted(map(tuple, open_orders[:, :3].tolist())),
                         sorted([(1, bid[0] - 2, 3), (-1, ask[0] + 2, 4)]))
        position = names.index("position")
        self.assertEqual(obs["state"][1, position], 2)
        # A quote at a price the exchange would not take pulls the one there.
        obs, *_ = pool.step_orders(np.array([[bid[0] - 2, 3, 2_000_000_000, 4, 0], [0, 0, 0, 0, 0]],
                                            dtype=np.int64))
        resting = obs["orders"][0][(obs["orders"][0][:, 2] > 0) & (obs["orders"][0][:, 4] == 0)]
        self.assertEqual(sorted(resting[:, 0].tolist()), [1])
        # Asking for no quotes cancels them.
        obs, *_ = pool.step_orders(np.zeros((2, 5), dtype=np.int64))
        self.assertTrue(np.all(obs["orders"][0][:, 2] == 0) or np.all(obs["orders"][0][:, 4] == 1))
        with self.assertRaises(ValueError):
            pool.step_orders(np.zeros((2, 4), dtype=np.int64))

    def test_a_take_can_carry_a_limit_price(self):
        pool = crowdbook_learn.Pool(MARKET, markets=2, threads=1)
        obs, _ = pool.reset(seeds=1)
        names = crowdbook_learn.STATE_NAMES
        ask = obs["state"][:, names.index("ask_price")].copy()
        # Market 0 bids for two lots a tick below the offer, which cannot fill; market 1 at it.
        orders = np.array([[0, 0, 0, 0, 2, ask[0] - 1], [0, 0, 0, 0, 2, ask[1]]], dtype=np.int64)
        obs, *_ = pool.step_orders(orders)
        position = names.index("position")
        self.assertEqual(obs["state"][0, position], 0)
        self.assertEqual(obs["state"][1, position], 2)

    def test_a_strategy_is_evaluated_across_markets_and_scales(self):
        from crowdbook_learn import evaluate
        state = crowdbook_learn.STATE_NAMES

        def join(observation, info):
            rows = observation["state"]
            bid, ask = rows[:, state.index("bid_price")], rows[:, state.index("ask_price")]
            ones = np.ones(len(rows), dtype=np.int64)
            return np.stack([bid, ones, ask, ones, 0 * ones], axis=1)

        text = short(MARKET.read_text())
        result = evaluate.evaluate(join, [text, text], seeds=range(1, 7), scales=(1, 2),
                                   markets=4, threads=1)
        once, twice = result[1], result[2]
        self.assertEqual(once["summary"]["markets"], 6)
        self.assertEqual([e.seed for e in once["episodes"]], list(range(1, 7)))
        self.assertEqual([e.scenario for e in once["episodes"]], [0, 1, 0, 1, 0, 1])
        self.assertGreater(once["summary"]["lots"], 0)
        self.assertAlmostEqual(once["summary"]["maker_share"], 1.0)
        self.assertIn("10s", once["summary"]["markouts"])
        self.assertGreater(once["summary"]["lots_ahead_median"], 0)  # joining the touch
        self.assertTrue(0 < once["summary"]["quote_fill_rate"] <= 1)
        self.assertGreaterEqual(twice["summary"]["lots"], once["summary"]["lots"])
        # The same seeds and policy give the same episodes.
        again = evaluate.run(join, [text, text], seeds=range(1, 7), markets=3, threads=2)
        self.assertEqual([e.score for e in again], [e.score for e in once["episodes"]])
        # A directory of scenario files stands for its .toml files, in name order.
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as scratch:
            for name in ("b.toml", "a.toml"):
                (Path(scratch) / name).write_text(text)
            (Path(scratch) / "notes.md").write_text("not a scenario")
            self.assertEqual(evaluate.scenario_texts(Path(scratch)), [text, text])
            from_files = evaluate.run(join, Path(scratch), seeds=range(1, 7), markets=3,
                                      threads=2)
        self.assertEqual([e.score for e in from_files], [e.score for e in once["episodes"]])

    def test_results_depend_on_seeds_and_actions_not_threads(self):
        text = short(MARKET.read_text())
        pools = [crowdbook_learn.Pool.from_text(text, 4, threads) for threads in (1, 4)]
        for pool in pools:
            pool.reset(seeds=[10, 11, 12, 13], settings_seeds=[1, 1, 2, 2])
        ended = 0
        for step in range(30):
            actions = actions_for(step, 4, len(pools[0].action_names), 2)
            results = [pool.step(actions) for pool in pools]
            for name in results[0][0]:
                np.testing.assert_array_equal(results[0][0][name], results[1][0][name])
            np.testing.assert_array_equal(results[0][1], results[1][1])
            np.testing.assert_array_equal(results[0][4]["score"], results[1][4]["score"])
            np.testing.assert_array_equal(results[0][4]["settings"], results[1][4]["settings"])
            done = results[0][2].copy()
            if done.any():
                ended += int(done.sum())
                for pool in pools:
                    pool.reset(seeds=100 + step * 4 + np.arange(4), mask=done)
        self.assertGreater(ended, 4)
        # The same settings seed drew the same settings.
        settings = pools[0].info["settings"]
        self.assertTrue(np.isfinite(settings).all())

    def test_a_copy_goes_on_as_its_source_does(self):
        pool = crowdbook_learn.Pool(MARKET, 3, threads=2)
        pool.reset(seeds=[5, 6, 7])
        actions_count = len(pool.action_names)
        for step in range(8):
            pool.step(actions_for(step, 3, actions_count, 2))
        untouched = pool.observation["state"][1].copy()
        observation, info = pool.copy(0, [2])
        np.testing.assert_array_equal(observation["state"][2], observation["state"][0])
        np.testing.assert_array_equal(observation["events"][2], observation["events"][0])
        np.testing.assert_array_equal(info["settings"][2], info["settings"][0])
        np.testing.assert_array_equal(info["seeds"][2], info["seeds"][0])
        np.testing.assert_array_equal(observation["state"][1], untouched)
        self.assertEqual(pool.session(2), pool.session(0))
        for step in range(8, 20):
            actions = actions_for(step, 3, actions_count, 2)
            actions[2] = actions[0]
            observation, rewards, *_ = pool.step(actions)
            np.testing.assert_array_equal(observation["state"][2], observation["state"][0])
            self.assertEqual(rewards[2], rewards[0])
        with self.assertRaises(ValueError):
            pool.copy(0, [0])
        with self.assertRaises(IndexError):
            pool.copy(0, [3])

    def test_stepping_releases_the_interpreters_lock(self):
        # The pool steps in another thread while this one counts. With the lock released for
        # each step, the count keeps going as fast as it does alone; held, this thread waits for
        # every step, and runs only in the short switches between them, a hundredth as often.
        # The best of three tries, so that a burst of load on the machine cannot fail it.
        pool = crowdbook_learn.Pool(MARKET, markets=32, threads=1)
        pool.reset(seeds=0)
        holds = np.zeros((32, 2), dtype=np.int64)

        def ratio() -> float:
            alone, began = 0, time.perf_counter()
            while time.perf_counter() - began < 0.05:
                alone += 1
            rate = alone / (time.perf_counter() - began)
            done = threading.Event()

            def steps():
                for _ in range(5):
                    pool.step(holds)
                done.set()

            stepper = threading.Thread(target=steps)
            counted, began = 0, time.perf_counter()
            stepper.start()
            while not done.is_set():
                counted += 1
            elapsed = time.perf_counter() - began
            stepper.join()
            return counted / (rate * elapsed)

        switching = sys.getswitchinterval()
        sys.setswitchinterval(1e-4)
        try:
            best = max(ratio() for _ in range(3))
        finally:
            sys.setswitchinterval(switching)
        self.assertGreater(best, 0.25)

    def test_calls_from_several_threads_take_turns(self):
        # Two threads stepping one pool end where one thread stepping as often does.
        text = MARKET.read_text()
        shared, alone = (crowdbook_learn.Pool.from_text(text, 8, 2) for _ in range(2))
        for pool in (shared, alone):
            pool.reset(seeds=3)
        holds = np.zeros((8, 2), dtype=np.int64)
        failures = []

        def steps():
            try:
                for _ in range(10):
                    shared.step(holds)
                    shared.session(0)
            except Exception as error:  # noqa: BLE001 - reported below
                failures.append(error)

        threads = [threading.Thread(target=steps) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(failures, [])
        for _ in range(20):
            alone.step(holds)
        for name in shared.observation:
            np.testing.assert_array_equal(shared.observation[name], alone.observation[name])

    def test_mistakes_are_reported(self):
        pool = crowdbook_learn.Pool(MARKET, markets=2)
        with self.assertRaises(RuntimeError):
            pool.step(np.zeros((2, 2), dtype=np.int64))  # nothing has started
        with self.assertRaises(ValueError):
            pool.reset(seeds=[1, 2, 3])
        pool.reset(seeds=[1, 2])
        with self.assertRaises(ValueError):
            pool.step(np.array([[16, 0], [0, 0]]))
        with self.assertRaises(ValueError):
            pool.step(np.zeros(2, dtype=np.int64))  # two sizes: a pair is needed
        with self.assertRaises(crowdbook_learn.ScenarioError):
            crowdbook_learn.Pool.from_text(
                "[learning]\nspeed = 1\n[[agents]]\ntype = \"momentum\"\n", 1)
        self.assertTrue(issubclass(crowdbook_learn.ScenarioError, ValueError))
        with self.assertRaises(TypeError):
            pool.step(np.array([[1.5, 0], [0, 0]]))  # actions are whole numbers
        with self.assertRaises(TypeError):
            pool.reset(seeds=[1.5, 2.0])
        with self.assertRaises(ValueError):
            pool.reset(seeds=[-1, 2])
        with self.assertRaises(ValueError):
            pool.reset(seeds=-1)
        with self.assertRaises(TypeError):
            pool.reset(seeds=1, scenarios="seed = 1")  # one text a market, not one text
        with self.assertRaisesRegex(IndexError, "no market 7"):
            pool.session(7)

    def test_an_episode_is_a_session_that_crowdbook_replays(self):
        pool = crowdbook_learn.Pool.from_text(short(MARKET.read_text()), 1)
        pool.reset(seeds=5)
        for step in range(4):
            pool.step(actions_for(step, 1, len(pool.action_names), 2))
        session = pool.session(0)
        self.assertIn('seats = ["learner"]', session)
        if CROWDBOOK is None:
            self.skipTest("set CROWDBOOK to the crowdbook command to replay it")
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "episode.toml"
            path.write_text(session)
            replay = subprocess.run([CROWDBOOK, "replay", str(path)], capture_output=True,
                                    text=True, check=False)
            self.assertEqual(replay.returncode, 0, replay.stderr)
            self.assertIn("learner", replay.stdout)


@unittest.skipIf(gymnasium is None, "Gymnasium is not installed")
class VectorEnvTest(unittest.TestCase):
    def test_follows_gymnasiums_vector_api_without_autoreset(self):
        env = crowdbook_learn.VectorEnv(MARKET, num_envs=3, threads=2)
        self.assertEqual(env.metadata["autoreset_mode"],
                         gymnasium.vector.AutoresetMode.DISABLED)
        obs, info = env.reset(seed=7)
        self.assertTrue(env.observation_space.contains(obs))
        self.assertEqual(list(info["seeds"]), [7, 8, 9])
        actions = env.action_space.sample()
        self.assertEqual(actions.shape, (3, 2))
        obs, rewards, terminated, truncated, info = env.step(actions)
        self.assertTrue(env.observation_space.contains(obs))
        self.assertEqual(rewards.shape, (3,))
        self.assertFalse(truncated.any())
        obs, info = env.reset(seed=[20, 21, 22], options={"reset_mask": np.array([0, 1, 0])})
        self.assertEqual(list(info["seeds"]), [7, 21, 9])

        # A seeded environment draws the same seeds for resets given none, or None for a market.
        def drawn(seed):
            other = crowdbook_learn.VectorEnv(MARKET, num_envs=3, threads=1)
            other.reset(seed=seed)
            first = other.reset()[1]["seeds"].copy()
            second = other.reset(seed=[5, None, 6])[1]["seeds"].copy()
            other.close()
            with self.assertRaises(RuntimeError):
                other.step(other.action_space.sample())
            return first, second

        first, second = drawn(11)
        again = drawn(11)
        np.testing.assert_array_equal(first, again[0])
        np.testing.assert_array_equal(second, again[1])
        self.assertEqual((second[0], second[2]), (5, 6))
        env.close()


if __name__ == "__main__":
    unittest.main()
