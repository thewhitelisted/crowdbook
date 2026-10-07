#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/exchange.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

constexpr int kStepsPerSeed = 2'000;
constexpr AgentId kUnknownAgent = 99;

// Deliberately different limits, so size and position rejections happen often.
constexpr std::array kConfigs = {
    AccountConfig{.maxPosition = 15, .maxOrderQuantity = 10},
    AccountConfig{
        .initialCash = 50'000, .initialPosition = 10, .maxPosition = 40, .maxOrderQuantity = 25},
    AccountConfig{.initialCash = -5'000,
                  .initialPosition = -20,
                  .maxPosition = 1'000,
                  .maxOrderQuantity = 25},
    AccountConfig{.initialCash = 1'000, .maxPosition = 5, .maxOrderQuantity = 30},
    AccountConfig{.initialPosition = 3, .maxPosition = 100, .maxOrderQuantity = 30},
    AccountConfig{.maxPosition = 100, .maxOrderQuantity = 30},
};

struct OpenOrder {
    Side side = Side::Buy;
    Price price = 0;
    Quantity leaves = 0;
};

// What an agent can reconstruct from its own private events and nothing else.
class Ledger {
public:
    explicit Ledger(const AccountConfig& config)
        : cash_(config.initialCash), position_(config.initialPosition) {}

    void apply(const Event& event) {
        if (const auto* accepted = std::get_if<OrderAccepted>(&event)) {
            open_[accepted->clientOrderId] = {
                .side = accepted->side, .price = accepted->price, .leaves = accepted->quantity};
        } else if (const auto* modified = std::get_if<OrderModified>(&event)) {
            OpenOrder& order = open_.at(modified->clientOrderId);
            order.price = modified->price;
            order.leaves = modified->quantity;
        } else if (const auto* filled = std::get_if<OrderFilled>(&event)) {
            const Cash notional = filled->price * filled->quantity;
            position_ += filled->side == Side::Buy ? filled->quantity : -filled->quantity;
            cash_ += filled->side == Side::Buy ? -notional : notional;
            OpenOrder& order = open_.at(filled->clientOrderId);
            if (order.leaves - filled->quantity != filled->leavesQuantity) {
                throw std::logic_error("fill leaves quantity does not add up");
            }
            order.leaves = filled->leavesQuantity;
            if (order.leaves == 0) {
                open_.erase(filled->clientOrderId);
            }
        } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
            const auto order = open_.find(cancelled->clientOrderId);
            if (order == open_.end()) {
                throw std::logic_error("cancellation for an order the agent never saw open");
            }
            if (order->second.leaves != cancelled->quantity) {
                throw std::logic_error("cancelled quantity is not the open quantity");
            }
            open_.erase(order);
        }
    }

    [[nodiscard]] Cash cash() const { return cash_; }
    [[nodiscard]] Quantity position() const { return position_; }
    [[nodiscard]] const std::map<ClientOrderId, OpenOrder>& open() const { return open_; }

private:
    Cash cash_ = 0;
    Quantity position_ = 0;
    std::map<ClientOrderId, OpenOrder> open_;
};

void expectLedgersMatch(const Exchange& exchange, const std::map<AgentId, Ledger>& ledgers) {
    std::size_t openOrders = 0;
    for (const auto& [agent, ledger] : ledgers) {
        SCOPED_TRACE(std::format("agent {}", agent));
        const Account& account = exchange.account(agent);
        ASSERT_EQ(ledger.cash(), account.cash);
        ASSERT_EQ(ledger.position(), account.position);
        for (const auto& [clientOrderId, order] : ledger.open()) {
            const std::optional<OrderId> id = exchange.liveOrderId(agent, clientOrderId);
            ASSERT_TRUE(id.has_value()) << "client order " << clientOrderId;
            ASSERT_EQ(exchange.book().find(*id), (RestingOrder{.id = *id,
                                                               .owner = agent,
                                                               .side = order.side,
                                                               .price = order.price,
                                                               .remaining = order.leaves}));
        }
        openOrders += ledger.open().size();
    }
    ASSERT_EQ(openOrders, exchange.book().orderCount());
}

class ExchangeRandomTest : public ::testing::TestWithParam<std::uint64_t> {};

// Random requests from several agents. After every step the exchange must pass its own audit
// (including conservation of cash and shares), the public feed must agree with the book, and each
// agent's view rebuilt from its own events must match the exchange exactly.
TEST_P(ExchangeRandomTest, AccountsBooksAndEventsStayConsistent) {
    // Draws straight from mt19937_64 so a failing seed reproduces on every platform.
    std::mt19937_64 rng{GetParam()};
    const auto below = [&rng](std::uint64_t bound) { return rng() % bound; };
    const auto between = [&below](std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
    };

    Exchange exchange;
    std::map<AgentId, Ledger> ledgers;
    std::map<AgentId, ClientOrderId> nextClientOrderId;
    for (std::size_t i = 0; i < kConfigs.size(); ++i) {
        const auto agent = static_cast<AgentId>(i + 1);
        exchange.addAgent(agent, kConfigs[i]);
        ledgers.emplace(agent, Ledger{kConfigs[i]});
        nextClientOrderId[agent] = 1;
    }
    TopOfBook feed; // what a subscriber to the public market data believes
    std::vector<Event> events;

    for (int step = 0; step < kStepsPerSeed; ++step) {
        SCOPED_TRACE(std::format("seed {} step {}", GetParam(), step));
        const AgentId agent =
            below(50) == 0 ? kUnknownAgent : static_cast<AgentId>(1 + below(kConfigs.size()));

        // Usually one of the agent's open orders, otherwise an id it has never used.
        const auto pickClientOrderId = [&]() -> ClientOrderId {
            const auto ledger = ledgers.find(agent);
            if (ledger != ledgers.end() && !ledger->second.open().empty() && below(10) < 8) {
                auto order = ledger->second.open().begin();
                std::advance(order,
                             static_cast<std::ptrdiff_t>(below(ledger->second.open().size())));
                return order->first;
            }
            return 1'000'000 + below(1'000);
        };

        Request request;
        const std::uint64_t action = below(100);
        if (action < 60) {
            NewOrder order{.clientOrderId =
                               below(20) == 0 ? pickClientOrderId() : nextClientOrderId[agent]++,
                           .side = below(2) == 0 ? Side::Buy : Side::Sell};
            const std::uint64_t kind = below(10);
            if (kind < 2) {
                order.timeInForce = TimeInForce::ImmediateOrCancel;
            } else if (kind < 4) {
                order.type = OrderType::Market;
            } else if (kind < 5) {
                order.timeInForce = TimeInForce::PostOnly;
            }
            order.price = below(25) == 0 ? 0 : between(95, 105); // 0 is an invalid limit price
            order.quantity = between(0, 30); // 0 is invalid; large sizes hit size limits
            request = order;
        } else if (action < 80) {
            request = CancelOrder{.clientOrderId = pickClientOrderId()};
        } else {
            ModifyOrder modify{.clientOrderId = pickClientOrderId()};
            modify.price = below(25) == 0 ? 0 : between(95, 105);
            modify.quantity = between(0, 30);
            request = modify;
        }

        events.clear();
        exchange.handle(agent, request, events);

        // Every request gets a reply, and the first event always answers the sender.
        ASSERT_FALSE(events.empty());
        ASSERT_EQ(recipient(events.front()), agent);

        Quantity traded = 0;
        Quantity takerFilled = 0;
        for (const Event& event : events) {
            if (const std::optional<AgentId> to = recipient(event)) {
                if (*to == kUnknownAgent) {
                    continue; // only ever a rejection, and there is no account to check
                }
                ASSERT_NO_THROW(ledgers.at(*to).apply(event));
                const auto* filled = std::get_if<OrderFilled>(&event);
                if (filled != nullptr && filled->liquidity == Liquidity::Taker) {
                    takerFilled += filled->quantity;
                }
            } else if (const auto* trade = std::get_if<Trade>(&event)) {
                traded += trade->quantity;
            } else {
                feed = std::get<TopOfBook>(event);
            }
        }

        ASSERT_EQ(traded, takerFilled);
        // A post-only order never takes liquidity.
        if (const auto* order = std::get_if<NewOrder>(&request);
            order != nullptr && order->type == OrderType::Limit &&
            order->timeInForce == TimeInForce::PostOnly) {
            ASSERT_EQ(takerFilled, 0);
        }
        ASSERT_EQ(feed, exchange.topOfBook());
        ASSERT_EQ(exchange.audit(), std::nullopt);
        ASSERT_NO_FATAL_FAILURE(expectLedgersMatch(exchange, ledgers));
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ExchangeRandomTest, ::testing::Range<std::uint64_t>(1, 31));

} // namespace
} // namespace crowdbook
