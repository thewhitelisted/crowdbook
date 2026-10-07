#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "crowdbook/event_log.hpp"
#include "crowdbook/simulation.hpp"
#include "test_agents.hpp"

namespace crowdbook {
namespace {

using test::add;

constexpr int kTraders = 8;
constexpr Timestamp kStopTrading = 500 * kMillisecond;

// Trades at random around the last mid price it heard about: mostly limit orders, plus market
// orders and cancels and modifies of its own open orders. It stops acting at `stopTime`.
//
// Every random draw goes into a local before it is used. Function arguments are evaluated in an
// unspecified order, so drawing inside an argument list could consume the stream in a different
// order on another compiler.
class RandomTrader final : public Agent {
public:
    RandomTrader(Duration meanWait, Timestamp stopTime)
        : meanWait_(meanWait), stopTime_(stopTime) {}

    void onStart(AgentContext& context) override { scheduleNext(context); }

    void onWakeup(AgentContext& context, std::uint64_t /*tag*/) override {
        act(context);
        scheduleNext(context);
    }

    void onTopOfBook(AgentContext& /*context*/, const TopOfBook& top) override {
        if (top.bid && top.ask) {
            mid_ = (top.bid->price + top.ask->price) / 2;
        }
    }

private:
    void scheduleNext(AgentContext& context) {
        const double rate = 1.0 / static_cast<double>(meanWait_);
        const Duration wait = 1 + static_cast<Duration>(context.random().exponential(rate));
        if (context.now() + wait < stopTime_) {
            context.wakeAfter(wait);
        }
    }

    void act(AgentContext& context) {
        Random& random = context.random();
        const std::map<ClientOrderId, OwnOrder>& orders = context.ledger().orders();
        const std::uint64_t roll = random.below(100);
        if (roll < 25 && !orders.empty()) {
            const ClientOrderId target = pick(random, orders);
            context.cancel(target);
        } else if (roll < 35 && !orders.empty()) {
            const ClientOrderId target = pick(random, orders);
            const Price price = mid_ + random.uniformInt(-5, 5);
            const Quantity quantity = random.uniformInt(1, 10);
            context.modify(target, price, quantity);
        } else if (roll < 45) {
            const Side side = randomSide(random);
            const Quantity quantity = random.uniformInt(1, 5);
            context.submitMarket(side, quantity);
        } else {
            const Side side = randomSide(random);
            const Price offset = random.uniformInt(-2, 8); // mostly away from the mid
            const Quantity quantity = random.uniformInt(1, 10);
            context.submitLimit(side, side == Side::Buy ? mid_ - offset : mid_ + offset, quantity);
        }
    }

    static Side randomSide(Random& random) {
        return random.below(2) == 0 ? Side::Buy : Side::Sell;
    }

    static ClientOrderId pick(Random& random, const std::map<ClientOrderId, OwnOrder>& orders) {
        auto order = orders.begin();
        std::advance(order, static_cast<std::ptrdiff_t>(random.below(orders.size())));
        return order->first;
    }

    Duration meanWait_;
    Timestamp stopTime_;
    Price mid_ = 1'000;
};

// Once nothing is in flight, an agent's own view must agree with the exchange exactly.
void expectLedgerMatchesExchange(const Simulation& simulation, AgentId id) {
    SCOPED_TRACE(std::format("agent {}", id));
    const Ledger& ledger = simulation.ledger(id);
    const Account& account = simulation.exchange().account(id);
    EXPECT_EQ(ledger.cash(), account.cash);
    EXPECT_EQ(ledger.position(), account.position);
    EXPECT_EQ(ledger.openQuantity(Side::Buy), account.openBuyQuantity);
    EXPECT_EQ(ledger.openQuantity(Side::Sell), account.openSellQuantity);
    for (const auto& [clientOrderId, order] : ledger.orders()) {
        EXPECT_TRUE(order.acknowledged);
        EXPECT_EQ(simulation.exchange().liveOrderId(id, clientOrderId), order.orderId);
        const std::optional<RestingOrder> resting =
            simulation.exchange().book().find(order.orderId);
        ASSERT_TRUE(resting.has_value());
        EXPECT_EQ(resting->price, order.price);
        EXPECT_EQ(resting->remaining, order.leaves);
    }
}

// Runs eight random traders with different latencies, all with jitter, and returns the CSV log.
std::string runMarket(std::uint64_t seed) {
    std::ostringstream log;
    CsvEventLog sink{log};
    Simulation simulation{seed};
    simulation.setEventSink(&sink);
    for (int i = 0; i < kTraders; ++i) {
        const Duration delay = (10 + 5 * i) * kMicrosecond;
        add<RandomTrader>(simulation,
                          {.account = {.maxPosition = 40, .maxOrderQuantity = 10},
                           .latency = {.toExchange = delay,
                                       .fromExchange = delay,
                                       .jitter = 20 * kMicrosecond},
                           .startTime = i * kMillisecond},
                          kMillisecond, kStopTrading);
    }
    // Run well past the last action, so every message in flight has landed.
    simulation.runUntil(kStopTrading + kSecond);

    EXPECT_EQ(simulation.pendingCount(), 0U);
    EXPECT_EQ(simulation.exchange().audit(), std::nullopt);
    for (AgentId id = 1; id <= static_cast<AgentId>(kTraders); ++id) {
        expectLedgerMatchesExchange(simulation, id);
    }
    return log.str();
}

// How many rows of each kind a log has; the kind is the second column.
std::map<std::string, std::size_t> countKinds(const std::string& log) {
    std::map<std::string, std::size_t> counts;
    std::istringstream lines{log};
    std::string line;
    std::getline(lines, line); // header
    while (std::getline(lines, line)) {
        const std::size_t first = line.find(',');
        const std::size_t second = line.find(',', first + 1);
        ++counts[line.substr(first + 1, second - first - 1)];
    }
    return counts;
}

// Describes the first line where two logs differ, for a readable failure message.
std::string firstDifference(const std::string& a, const std::string& b) {
    std::istringstream left{a};
    std::istringstream right{b};
    std::string leftLine;
    std::string rightLine;
    for (int number = 1;; ++number) {
        const bool hasLeft = static_cast<bool>(std::getline(left, leftLine));
        const bool hasRight = static_cast<bool>(std::getline(right, rightLine));
        if (!hasLeft && !hasRight) {
            return "the logs are identical";
        }
        if (hasLeft != hasRight || leftLine != rightLine) {
            return std::format("line {}: '{}' vs '{}'", number, hasLeft ? leftLine : "<end>",
                               hasRight ? rightLine : "<end>");
        }
    }
}

TEST(SimulationDeterminismTest, SameSeedGivesAnIdenticalEventLog) {
    const std::string first = runMarket(7);
    const std::string second = runMarket(7);
    EXPECT_TRUE(first == second) << firstDifference(first, second);

    // The market has to be busy for the comparison to mean anything: every kind of row appears.
    const std::map<std::string, std::size_t> counts = countKinds(first);
    for (const char* kind : {"new", "cancel", "modify", "accepted", "rejected", "modified",
                             "filled", "cancelled", "trade", "top_of_book"}) {
        EXPECT_GT(counts.contains(kind) ? counts.at(kind) : 0U, 10U) << kind;
    }
}

TEST(SimulationDeterminismTest, DifferentSeedsGiveDifferentLogs) {
    EXPECT_TRUE(runMarket(7) != runMarket(8));
}

} // namespace
} // namespace crowdbook
