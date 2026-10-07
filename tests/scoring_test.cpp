#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "crowdbook/scoring.hpp"

namespace crowdbook {
namespace {

constexpr AgentId kSeat = 7;
constexpr AgentId kOther = 8;

OrderFilled fill(AgentId agent, Side side, Price price, Quantity quantity, Fee fee = 0) {
    return {.agent = agent, .side = side, .price = price, .quantity = quantity, .fee = fee};
}

Trade trade(Price price, Quantity quantity) {
    return {.price = price, .quantity = quantity};
}

// The seat buys 2 at 100 a second in, paying 0.3 in fees; someone else trades 1 at 105 at 3s.
void playScript(Scorer& scorer) {
    scorer.addSeat(kSeat, 0, 0);
    scorer.onEvent(kSecond, fill(kOther, Side::Sell, 100, 2));
    scorer.onEvent(kSecond, fill(kSeat, Side::Buy, 100, 2, 300));
    scorer.onEvent(kSecond, trade(100, 2));
    scorer.onEvent(3 * kSecond, fill(kOther, Side::Buy, 105, 1));
    scorer.onEvent(3 * kSecond, trade(105, 1));
}

TEST(ScoringTest, PnlIsNetOfFeesAtTheMark) {
    Scorer scorer{{}, 100};
    playScript(scorer);
    // -200 cash, 2 lots at 105, 0.3 in fees: 9.7 tick-lots.
    const Score score = scorer.score(kSeat, 5 * kSecond, std::nullopt);
    EXPECT_EQ(score.pnl, 9'700);
    EXPECT_EQ(score.total, 9'700);
    EXPECT_FALSE(score.stoppedAt);

    Scorer atValue{{.mark = Mark::Value}, 100};
    playScript(atValue);
    EXPECT_EQ(atValue.score(kSeat, 5 * kSecond, 103.5).pnl, 6'700); // 2 lots at 103.5
    EXPECT_THROW(static_cast<void>(atValue.score(kSeat, 5 * kSecond, std::nullopt)),
                 std::invalid_argument);
}

TEST(ScoringTest, HoldingAndClosingCost) {
    Scorer scorer{{.inventoryPenalty = 100, .closePenalty = 1'000}, 100};
    playScript(scorer);
    // Two lots from 1s to 5s is eight lot-seconds.
    const Score score = scorer.score(kSeat, 5 * kSecond, std::nullopt);
    EXPECT_EQ(score.inventory, 800);
    EXPECT_EQ(score.close, 2'000);
    EXPECT_EQ(score.total, 9'700 - 800 - 2'000);
}

TEST(ScoringTest, InventoryFollowsEveryChangeOfPosition) {
    Scorer scorer{{.inventoryPenalty = 1'000}, 100};
    scorer.addSeat(kSeat, 0, 3); // starts holding 3
    scorer.onEvent(2 * kSecond, fill(kSeat, Side::Sell, 100, 5)); // to -2
    scorer.onEvent(4 * kSecond, fill(kSeat, Side::Buy, 100, 2));  // to 0
    // 3 lots for 2s, 2 lots for 2s, then none: 10 lot-seconds.
    EXPECT_EQ(scorer.score(kSeat, 9 * kSecond, std::nullopt).inventory, 10'000);
}

TEST(ScoringTest, WhatIsHeldAtTheEndCostsWhereverItCameFrom) {
    Scorer scorer{{.closePenalty = 500}, 100};
    scorer.addSeat(kSeat, 0, -3); // starts short 3 and never trades
    EXPECT_EQ(scorer.score(kSeat, kSecond, std::nullopt).close, 1'500);
}

TEST(ScoringTest, ATargetIsMeasuredAgainstItsPaperPortfolio) {
    const Target target{.side = Side::Buy,
                        .quantity = 5,
                        .benchmark = Benchmark::Vwap,
                        .unfinishedPenalty = 2'000};
    Scorer scorer{{.target = target}, 100};
    playScript(scorer);
    const Score score = scorer.score(kSeat, 5 * kSecond, std::nullopt);
    // VWAP is (200 + 105) / 3; five lots bought there are worth 5 × (105 − 101.667) at the mark.
    EXPECT_EQ(score.paper, 16'667);
    EXPECT_EQ(score.unfinishedLots, 3);
    EXPECT_EQ(score.unfinished, 6'000);
    EXPECT_EQ(score.total, 9'700 - 16'667 - 6'000);

    Scorer reference{{.target = Target{.side = Side::Buy, .quantity = 5,
                                       .benchmark = Benchmark::Reference}},
                     100};
    playScript(reference);
    EXPECT_EQ(reference.score(kSeat, 5 * kSecond, std::nullopt).paper, 25'000);
}

TEST(ScoringTest, ATargetThatIsDoneAtTheBenchmarkScoresItsFees) {
    // Selling 4 from a position of 4, all at the reference price, which is where the market ends.
    Scorer scorer{{.target = Target{.side = Side::Sell,
                                    .quantity = 4,
                                    .benchmark = Benchmark::Reference,
                                    .unfinishedPenalty = 9'000}},
                  50};
    scorer.addSeat(kSeat, 0, 4);
    scorer.onEvent(kSecond, fill(kSeat, Side::Sell, 50, 4, 100));
    scorer.onEvent(kSecond, trade(50, 4));
    const Score score = scorer.score(kSeat, 2 * kSecond, std::nullopt);
    EXPECT_EQ(score.unfinishedLots, 0);
    EXPECT_EQ(score.paper, 0);
    EXPECT_EQ(score.total, -100);
}

TEST(ScoringTest, TheLossLimitStopsASeatAtTheTradeThatReachesIt) {
    Scorer scorer{{.maxLoss = 4}, 100};
    scorer.addSeat(kSeat, 0, 0);
    scorer.addSeat(kOther, 0, 0);
    scorer.onEvent(kSecond, fill(kSeat, Side::Buy, 100, 2));
    scorer.onEvent(kSecond, trade(100, 2));
    scorer.onEvent(2 * kSecond, trade(99, 1)); // a loss of 2
    EXPECT_FALSE(scorer.stoppedAt(kSeat));
    scorer.onEvent(3 * kSecond, trade(98, 1)); // a loss of 4, the limit itself
    EXPECT_EQ(scorer.stoppedAt(kSeat), 3 * kSecond);
    scorer.onEvent(4 * kSecond, trade(97, 1));  // a deeper loss does not move the moment
    scorer.onEvent(5 * kSecond, trade(120, 1)); // and a recovery does not undo it
    EXPECT_EQ(scorer.score(kSeat, 6 * kSecond, std::nullopt).stoppedAt, 3 * kSecond);
    EXPECT_FALSE(scorer.stoppedAt(kOther));
}

TEST(ScoringTest, OnlySeatsAreScored) {
    Scorer scorer{{}, 100};
    scorer.addSeat(kSeat, 0, 0);
    scorer.onEvent(kSecond, fill(kOther, Side::Buy, 100, 2));
    EXPECT_EQ(scorer.score(kSeat, kSecond, std::nullopt).pnl, 0);
    EXPECT_THROW(static_cast<void>(scorer.score(kOther, kSecond, std::nullopt)),
                 std::out_of_range);
}

TEST(ScoringTest, SettingsAreChecked) {
    EXPECT_THROW(Scorer({.inventoryPenalty = -1}, 100), std::invalid_argument);
    EXPECT_THROW(Scorer({.maxLoss = -1}, 100), std::invalid_argument);
    EXPECT_THROW(Scorer({.target = Target{.quantity = 0}}, 100), std::invalid_argument);
}

} // namespace
} // namespace crowdbook
