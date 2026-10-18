"""The no-false-edge check's learning probe (analysis/src/crowdbook_analysis/learner.py) in the
engine's own markets, with a known answer: where the price follows the value late, a learner
trained there takes it; where it follows at once, the learner learns to stay out. CTest runs these
with the built module on PYTHONPATH and CROWDBOOK_SOURCE_DIR set."""

import os
import pathlib
import sys
import unittest

import crowdbook_learn

SOURCE = pathlib.Path(os.environ.get("CROWDBOOK_SOURCE_DIR",
                                     pathlib.Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(SOURCE / "analysis" / "src"))

from crowdbook_analysis import learner  # noqa: E402  (needs NumPy alone)

# A value that wanders, one maker quoting it `lag` late, and sharp traders who see it at once and
# take when the book is two ticks off it: their orders say where the value went before the
# maker's quotes do.
MARKET = """
seed = 1
duration = "120s"
reference_price = 10000

[fundamental]
volatility = 1.0
step = "100ms"

[exchange]
depth_levels = 5
taker_fee = 0.1

[[agents]]
type = "market_maker"
name = "maker"
quote_value = true
noise = 0.0
lag = "LAG"
intensity = 1.0
risk_aversion = 0.0001
volatility = 1.0
quote_size = 20
max_inventory = 100000
requote_interval = "200ms"
post_only = true
account = { max_position = 200000, max_order_quantity = 1000 }

[[agents]]
type = "informed"
name = "sharps"
count = 3
interval = "500ms"
noise = 0.3
threshold = 2.0
order_size = 2
max_position = 1000
account = { max_position = 1000, max_order_quantity = 100 }
"""


def trained(lag: str) -> dict:
    settings = learner.Settings(generations=12, markets=8, threads=2)
    texts = [learner.seat(MARKET.replace("LAG", lag), latency=settings.latency,
                          size=settings.size, max_position=settings.max_position,
                          interval=settings.interval)]
    weights, _ = learner.train(crowdbook_learn, texts, settings, log=lambda _: None)
    return learner.summary(learner.judge(crowdbook_learn, texts, weights, 200, settings))


class LearnerProbeTest(unittest.TestCase):
    def test_takes_what_a_late_price_offers_and_nothing_where_it_is_prompt(self):
        late = trained("10s")
        self.assertGreater(late["low"], 0, late)
        self.assertGreater(late["lots"], 1, late)
        prompt = trained("0s")
        self.assertFalse(prompt["low"] > 0, prompt)
        self.assertLess(prompt["mean"], late["low"], prompt)


if __name__ == "__main__":
    unittest.main()
