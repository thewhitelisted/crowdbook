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

        # The same arrays, at the same addresses, after every step.
        address = obs["events"].__array_interface__["data"][0]
        before = obs["state"].copy()
        returned = pool.step(np.zeros((3, 2), dtype=np.int64))
        self.assertIs(returned[0]["events"], obs["events"])
        self.assertEqual(obs["events"].__array_interface__["data"][0], address)
        np.testing.assert_array_equal(obs["state"][:, time], 6_000_000_000)
        self.assertFalse(np.array_equal(before, obs["state"]))
        self.assertEqual(pool.action_names[0], "hold")
        self.assertEqual(pool.sizes, [1, 5])

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

    def test_stepping_releases_the_interpreters_lock(self):
        # The pool steps in another thread while this one counts. With the lock released for
        # each step, the count keeps going as fast as it does alone; held, this thread waits for
        # every step, and runs only in the short switches between them.
        pool = crowdbook_learn.Pool(MARKET, markets=32, threads=1)
        pool.reset(seeds=0)
        holds = np.zeros((32, 2), dtype=np.int64)
        switching = sys.getswitchinterval()
        sys.setswitchinterval(1e-4)
        try:
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
        finally:
            sys.setswitchinterval(switching)
        self.assertGreater(counted, 0.25 * rate * elapsed)

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
            crowdbook_learn.Pool.from_text("[learning]\nspeed = 1\n[[agents]]\ntype = \"momentum\"\n",
                                           1)
        self.assertTrue(issubclass(crowdbook_learn.ScenarioError, ValueError))

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


if __name__ == "__main__":
    unittest.main()
