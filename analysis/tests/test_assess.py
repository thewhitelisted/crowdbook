import unittest

from crowdbook_analysis import assess, sweep


def report(reached: bool) -> dict:
    """A fit's report data, as crowdbook-calibrate writes it, with two compared statistics."""
    def statistic(key, real, value, distance, passes):
        return {"key": key, "label": key.replace("_", " "), "real": real,
                "simulated": [{"value": value * 2, "distance": distance * 3, "pass": False},
                              {"value": value, "distance": distance, "pass": passes}]}
    return {
        "calibration": {"compared": ["spread_mean", "orders_per_minute"], "reached": reached,
                        "distance_held_out": 4.0 if reached else 900.0, "runs": 1234},
        "statistics": [statistic("spread_mean", 3.4, 3.5, 0.8, True),
                       statistic("orders_per_minute", 0.1, 2.0, 25.0, False),
                       statistic("quote_1_after_60", 0.5, 1.5, 4.0, False)],
    }


def swept(flagged: bool) -> dict:
    maker = sweep.Probe.of("maker", {"offset": 1.0})
    trend = sweep.Probe.of("trend", {"threshold": 2.0})
    paid = {"mean": 12.0, "low": 3.0 if flagged else -2.0, "high": 21.0}
    nothing = {"mean": -1.0, "low": -3.0, "high": 1.0}
    return {"searched": {}, "judged": {maker: paid, trend: nothing}}


class VerdictTest(unittest.TestCase):
    def test_usable_only_when_the_fit_reaches_and_nothing_is_paid(self):
        text, usable = assess.verdict(report(True), swept(False), 100, 400)
        self.assertTrue(usable)
        self.assertIn("**Usable for testing strategies**", text)
        self.assertIn("1 of 2 compared statistics", text)
        # The statistic furthest from the recording comes first; one not compared is left out.
        self.assertLess(text.index("orders per minute"), text.index("spread mean"))
        self.assertNotIn("| quote 1 after 60 |", text)
        self.assertIn("| 1 ticks out, marked 60 s later | +0.50 | +1.50 |", text)

    def test_says_why_a_fit_is_not_usable(self):
        text, usable = assess.verdict(report(False), swept(True), 100, 400)
        self.assertFalse(usable)
        self.assertIn("does not reach the recording within chance", text)
        self.assertIn("pays a strategy: maker (offset 1.0)", text)
        self.assertIn("**edge**", text)
        _, unchecked = assess.verdict(report(True), None, 100, 400)
        self.assertFalse(unchecked)

    def test_an_edge_check_that_judged_nothing_is_not_a_pass(self):
        nothing = {"searched": {}, "judged": {}}
        text, usable = assess.verdict(report(True), nothing, 100, 400)
        self.assertFalse(usable)
        self.assertIn("judged no strategy with an interval", text)
        # One fresh market gives no interval: a profit there is not a pass either.
        maker = sweep.Probe.of("maker", {"offset": 1.0})
        one = {"searched": {}, "judged": {maker: {"mean": 25.0, "low": float("nan"),
                                                  "high": float("nan")}}}
        self.assertFalse(assess.verdict(report(True), one, 100, 1)[1])

    def test_a_statistic_with_no_distance_is_left_out_of_the_worst(self):
        data = report(True)
        data["statistics"][0]["simulated"][-1]["distance"] = float("nan")
        text, _ = assess.verdict(data, swept(False), 100, 400)
        self.assertNotIn("nan", text.split("## Edges")[0])
        self.assertIn("orders per minute", text)


if __name__ == "__main__":
    unittest.main()
