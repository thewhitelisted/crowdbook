#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/adaptive.hpp"
#include "exchange_test_support.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;

constexpr Price kReference = 100;

std::shared_ptr<Fundamental> fixedValue(double value) {
    return std::make_shared<Fundamental>(FundamentalConfig{.initial = value, .volatility = 0.0},
                                         Random{1, 0});
}

// Sees the value exactly, and always follows whichever record is better.
const AdaptiveConfig kSure{.interval = kSecond,
                           .noise = 0.0,
                           .fastHalfLife = kSecond,
                           .slowHalfLife = 10 * kSecond,
                           .memory = 30 * kSecond,
                           .choiceIntensity = 1e9,
                           .threshold = 1.0,
                           .orderSize = 2,
                           .maxPosition = 1'000};

// Shows the trader a two-tick market around `mid` at `time` and wakes it; returns what it sent.
std::vector<Request> look(AdaptiveTrader& trader, FakeContext& context, Timestamp time,
                          Price mid) {
    context.setNow(time);
    context.snapshot = {.bid = LevelSummary{.price = mid - 1, .quantity = 1},
                        .ask = LevelSummary{.price = mid + 1, .quantity = 1}};
    trader.onWakeup(context, 0);
    return context.takeSent();
}

Side sideOf(const Request& request) { return std::get<NewOrder>(request).side; }

TEST(AdaptiveTest, FollowsTheTrendWhileTheTrendKeepsPaying) {
    // The price climbs steadily away from a value of 100: the value strategy keeps calling sells
    // that lose, the trend strategy buys that win.
    AdaptiveTrader trader{kSure, fixedValue(100.0), kReference};
    FakeContext context;
    std::vector<Request> last;
    for (int i = 0; i < 20; ++i) {
        last = look(trader, context, (i + 1) * kSecond, 104 + 2 * i);
    }
    EXPECT_TRUE(trader.followingTrend());
    EXPECT_GT(trader.trendRecord(), trader.valueRecord());
    ASSERT_EQ(last.size(), 1U);
    EXPECT_EQ(sideOf(last[0]), Side::Buy);
}

TEST(AdaptiveTest, FollowsTheValueWhenThePriceKeepsComingBack) {
    // The price swings either side of the value and back, so trends reverse and value calls win.
    AdaptiveTrader trader{kSure, fixedValue(100.0), kReference};
    FakeContext context;
    const std::vector<Price> swing{106, 103, 100, 97, 94, 97, 100, 103};
    for (int i = 0; i < 48; ++i) {
        const Price mid = swing[static_cast<std::size_t>(i) % swing.size()];
        static_cast<void>(look(trader, context, (i + 1) * kSecond, mid));
    }
    const std::vector<Request> sent = look(trader, context, 49 * kSecond, 106);
    EXPECT_FALSE(trader.followingTrend());
    EXPECT_GT(trader.valueRecord(), trader.trendRecord());
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sideOf(sent[0]), Side::Sell); // 6 ticks above the value
}

TEST(AdaptiveTest, TrackRecordsFadeWithTheirHalfLife) {
    AdaptiveTrader trader{kSure, fixedValue(100.0), kReference};
    FakeContext context;
    for (int i = 0; i < 20; ++i) {
        static_cast<void>(look(trader, context, (i + 1) * kSecond, 104 + 2 * i));
    }
    // A flat market scores nothing, so for one memory half-life the records only fade.
    static_cast<void>(look(trader, context, 21 * kSecond, 142));
    const double record = trader.trendRecord();
    ASSERT_GT(record, 10.0);
    for (int i = 0; i < 30; ++i) {
        static_cast<void>(look(trader, context, (22 + i) * kSecond, 142));
    }
    EXPECT_NEAR(trader.trendRecord(), record / 2.0, record * 0.01);
}

TEST(AdaptiveTest, ChoosesAtRandomWhenRecordsAreLevelAndTradesTowardItsTarget) {
    AdaptiveConfig config = kSure;
    config.choiceIntensity = 0.0; // a coin toss every time
    config.maxPosition = 4;
    AdaptiveTrader trader{config, fixedValue(120.0), kReference};
    FakeContext context;
    std::set<bool> choices;
    for (int i = 0; i < 40; ++i) {
        static_cast<void>(look(trader, context, (i + 1) * kSecond, 100));
        choices.insert(trader.followingTrend());
        // Nothing fills here, so its orders in flight are its exposure: never past the limit.
        const Ledger& ledger = context.ledger();
        const Quantity exposure = ledger.openQuantity(Side::Buy) - ledger.openQuantity(Side::Sell);
        EXPECT_GE(exposure, 0); // the value strategy wants +4, the flat trend strategy 0
        EXPECT_LE(exposure, 4);
    }
    EXPECT_EQ(choices.size(), 2U);
}

TEST(AdaptiveTest, BuildsItsPositionInStepsOfOrderSize) {
    AdaptiveConfig config = kSure;
    config.choiceIntensity = 0.0;
    config.orderSize = 3;
    config.maxPosition = 7;
    // The value is far above and the price flat, so the value strategy wants to be long 7 and
    // the trend strategy flat; with records level it alternates at random.
    AdaptiveTrader trader{config, fixedValue(150.0), kReference};
    FakeContext context;
    Quantity bought = 0;
    for (int i = 0; i < 6; ++i) {
        for (const Request& request : look(trader, context, (i + 1) * kSecond, 100)) {
            const auto& order = std::get<NewOrder>(request);
            EXPECT_LE(order.quantity, 3);
            bought += order.side == Side::Buy ? order.quantity : -order.quantity;
        }
    }
    EXPECT_LE(bought, 7);
}

TEST(AdaptiveTest, FirstLooksAtARandomPointInItsFirstInterval) {
    const std::vector<Timestamp> first = test::firstWakeups(
        [] { return std::make_unique<AdaptiveTrader>(kSure, fixedValue(100.0), kReference); }, 20);
    EXPECT_GT(std::set<Timestamp>(first.begin(), first.end()).size(), 15U);
    for (const Timestamp time : first) {
        EXPECT_GT(time, 0);
        EXPECT_LE(time, kSure.interval);
    }
}

TEST(AdaptiveTest, RequiresAFundamentalAndAValidConfig) {
    EXPECT_THROW((AdaptiveTrader{kSure, nullptr, kReference}), std::invalid_argument);
    const auto invalid = [](auto change) {
        AdaptiveConfig config = kSure;
        change(config);
        return config;
    };
    for (const AdaptiveConfig& config :
         {invalid([](AdaptiveConfig& c) { c.interval = 0; }),
          invalid([](AdaptiveConfig& c) { c.memory = 0; }),
          invalid([](AdaptiveConfig& c) { c.noise = -1.0; }),
          invalid([](AdaptiveConfig& c) { c.choiceIntensity = -1.0; }),
          invalid([](AdaptiveConfig& c) { c.slowHalfLife = c.fastHalfLife; }),
          invalid([](AdaptiveConfig& c) { c.orderSize = 0; }),
          invalid([](AdaptiveConfig& c) { c.maxPosition = 1; })}) {
        EXPECT_THROW((AdaptiveTrader{config, fixedValue(100.0), kReference}),
                     std::invalid_argument);
    }
}

} // namespace
} // namespace crowdbook
