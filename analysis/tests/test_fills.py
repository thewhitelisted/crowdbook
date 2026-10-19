import tempfile
import unittest
from pathlib import Path

import numpy as np

from crowdbook_analysis import fills, tape


def measure(episodes: list) -> dict:
    """A stand-in for the harness's summary, with its keys: lots a market and the share of
    them resting, from the fills alone."""
    lots = [sum(f[3] for f in e.fills) for e in episodes]
    rested = sum(f[3] for e in episodes for f in e.fills if f[4])
    total = sum(lots)
    return {"quote_fill_rate": np.nan, "lots_ahead_median": np.nan,
            "wait_to_fill_median": np.nan,
            "maker_share": rested / total if total else np.nan,
            "lots": float(np.mean(lots)), "markouts": {"1s": np.nan}}


def episode(lots: list[int], maker: bool = True) -> fills.PaperEpisode:
    return fills.PaperEpisode(seed=0, scenario=0,
                              fills=[(k, 1, 50, q, maker, 0) for k, q in enumerate(lots)])


class PaperTest(unittest.TestCase):
    def test_a_paper_record_reads_as_episodes_in_ticks_and_nanoseconds(self):
        book = np.full((3, 1), 0.49)
        market = tape.Market(name="m", duration=10.0, book_time=np.array([0.0, 1.0, 2.0]),
                             bid_price=book, bid_size=np.full((3, 1), 5.0),
                             ask_price=book + 0.02, ask_size=np.full((3, 1), 5.0),
                             trade_time=np.array([]), trade_price=np.array([]),
                             trade_size=np.array([]), trade_sign=np.array([]))
        with tempfile.TemporaryDirectory() as scratch:
            tape.write_tape(tape.Tape([market], tape.Options(tick=0.01, payout=1.0)), scratch)
            (Path(scratch) / "quotes.csv").write_text(
                "market,time,side,price,size,ahead\nm,1.5,buy,0.49,3,40\n")
            (Path(scratch) / "fills.csv").write_text(
                "market,time,side,price,quantity,liquidity\nm,2.25,buy,0.49,2,maker\n"
                "m,3,sell,0.51,1,taker\n")
            (paper,) = fills.read_paper(scratch)
        self.assertEqual(paper.quotes, [(1_500_000_000, 1, 49, 3, 40)])
        self.assertEqual(paper.fills, [(2_250_000_000, 1, 49, 2, True, 0),
                                       (3_000_000_000, -1, 51, 1, False, 0)])
        self.assertEqual(paper.mids[0], (0, 50.0))

    def test_sides_and_liquidity_are_checked(self):
        with self.assertRaises(ValueError):
            fills._side(["buy", "long"])

    def test_figures_come_with_intervals_over_markets(self):
        figures = fills.summarize([episode([2]), episode([4]), episode([6])], draws=50,
                                  measure=measure)
        value, low, high = figures["lots"]
        self.assertEqual(value, 4.0)
        self.assertTrue(2.0 <= low <= value <= high <= 6.0)
        self.assertTrue(np.isnan(figures["quote_fill_rate"][0]))

    def test_names_the_figures_the_fit_gets_wrong(self):
        paper = [episode([1]), episode([2]), episode([1]), episode([2])]
        alike = [episode([1]), episode([2]), episode([2]), episode([1])]
        busy = [episode([20], maker=True), episode([25])] * 2
        text, apart = fills.compare(paper, alike, draws=50, measure=measure)
        self.assertEqual(apart, [])
        self.assertIn("Every figure's intervals overlap", text)
        text, apart = fills.compare(paper, busy, draws=50, measure=measure)
        self.assertEqual(apart, ["lots"])
        self.assertIn("unlike the real market**: lots traded a market", text)


if __name__ == "__main__":
    unittest.main()
