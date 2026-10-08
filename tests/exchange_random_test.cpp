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
            fees_ += filled->fee;
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
    [[nodiscard]] Fee fees() const { return fees_; }
    [[nodiscard]] const std::map<ClientOrderId, OpenOrder>& open() const { return open_; }

private:
    Cash cash_ = 0;
    Quantity position_ = 0;
    Fee fees_ = 0;
    std::map<ClientOrderId, OpenOrder> open_;
};

void expectLedgersMatch(const Exchange& exchange, const std::map<AgentId, Ledger>& ledgers) {
    std::size_t openOrders = 0;
    for (const auto& [agent, ledger] : ledgers) {
        SCOPED_TRACE(std::format("agent {}", agent));
        const Account& account = exchange.account(agent);
        ASSERT_EQ(ledger.cash(), account.cash);
        ASSERT_EQ(ledger.position(), account.position);
        ASSERT_EQ(ledger.fees(), account.fees);
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

    Exchange exchange{{.depthLevels = 3, .makerFee = -150, .takerFee = 400}};
    std::map<AgentId, Ledger> ledgers;
    std::map<AgentId, ClientOrderId> nextClientOrderId;
    for (std::size_t i = 0; i < kConfigs.size(); ++i) {
        const auto agent = static_cast<AgentId>(i + 1);
        exchange.addAgent(agent, kConfigs[i]);
        ledgers.emplace(agent, Ledger{kConfigs[i]});
        nextClientOrderId[agent] = 1;
    }
    TopOfBook feed;   // what a subscriber to the public market data believes
    BookDepth depth;  // and to the depth feed
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
            } else if (const auto* top = std::get_if<TopOfBook>(&event)) {
                feed = *top;
            } else {
                depth = std::get<BookDepth>(event);
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
        ASSERT_EQ(depth.bids, exchange.book().depth(Side::Buy, 3));
        ASSERT_EQ(depth.asks, exchange.book().depth(Side::Sell, 3));
        ASSERT_EQ(exchange.audit(), std::nullopt);
        ASSERT_NO_FATAL_FAILURE(expectLedgersMatch(exchange, ledgers));
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ExchangeRandomTest, ::testing::Range<std::uint64_t>(1, 31));

} // namespace
} // namespace crowdbook

namespace crowdbook {
namespace {

class TradingDayRandomTest : public ::testing::TestWithParam<std::uint64_t> {};

// The same random requests through a trading day that changes phase at random: auctions in which
// orders rest crossed and the book uncrosses, the close, and halts set off by trades outside a
// narrow band. After every step and every phase change the exchange must pass its audit, the
// public feed must agree with the book, the indicative price with a fresh calculation, and every
// agent's ledger with its account.
TEST_P(TradingDayRandomTest, AccountsBooksAndEventsStayConsistentThroughTheDay) {
    std::mt19937_64 rng{GetParam()};
    const auto below = [&rng](std::uint64_t bound) { return rng() % bound; };
    const auto between = [&below](std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
    };

    Exchange exchange{{.depthLevels = 3,
                       .makerFee = -150,
                       .takerFee = 400,
                       .auctionFee = 200,
                       .referencePrice = 100,
                       .haltBand = 3,
                       .haltDuration = kSecond}};
    std::map<AgentId, Ledger> ledgers;
    std::map<AgentId, ClientOrderId> nextClientOrderId;
    for (std::size_t i = 0; i < kConfigs.size(); ++i) {
        const auto agent = static_cast<AgentId>(i + 1);
        exchange.addAgent(agent, kConfigs[i]);
        ledgers.emplace(agent, Ledger{kConfigs[i]});
        nextClientOrderId[agent] = 1;
    }
    TopOfBook feed;
    BookDepth depth;
    std::optional<Uncross> indicative;
    std::vector<Event> events;
    int uncrosses = 0;
    int halts = 0;

    // Checks a batch of events and applies them to the subscribers' views.
    const auto take = [&](const std::vector<Event>& batch) {
        Quantity traded = 0;
        Quantity takerFilled = 0;
        Quantity auctionTraded = 0;
        Quantity auctionFilled = 0;
        for (const Event& event : batch) {
            if (const std::optional<AgentId> to = recipient(event)) {
                if (*to == kUnknownAgent) {
                    continue;
                }
                ASSERT_NO_THROW(ledgers.at(*to).apply(event));
                if (const auto* filled = std::get_if<OrderFilled>(&event)) {
                    (filled->liquidity == Liquidity::Auction ? auctionFilled : takerFilled) +=
                        filled->liquidity == Liquidity::Maker ? 0 : filled->quantity;
                }
            } else if (const auto* trade = std::get_if<Trade>(&event)) {
                (trade->auction ? auctionTraded : traded) += trade->quantity;
            } else if (const auto* top = std::get_if<TopOfBook>(&event)) {
                feed = *top;
            } else if (const auto* published = std::get_if<BookDepth>(&event)) {
                depth = *published;
            } else if (const auto* phase = std::get_if<PhaseChanged>(&event)) {
                ASSERT_EQ(phase->phase, exchange.phase());
                halts += phase->phase == Phase::HaltAuction ? 1 : 0;
                if (!isAuction(phase->phase)) {
                    indicative.reset();
                }
            } else {
                indicative = std::get<Indicative>(event).uncross;
            }
        }
        ASSERT_EQ(traded, takerFilled);
        ASSERT_EQ(2 * auctionTraded, auctionFilled); // both sides of every auction trade
        ASSERT_EQ(feed, exchange.topOfBook());
        ASSERT_EQ(depth.bids, exchange.book().depth(Side::Buy, 3));
        ASSERT_EQ(depth.asks, exchange.book().depth(Side::Sell, 3));
        if (isAuction(exchange.phase())) {
            ASSERT_EQ(indicative, exchange.book().indicative(exchange.reference()));
        }
        ASSERT_EQ(exchange.audit(), std::nullopt);
        ASSERT_NO_FATAL_FAILURE(expectLedgersMatch(exchange, ledgers));
    };

    for (int step = 0; step < kStepsPerSeed; ++step) {
        SCOPED_TRACE(std::format("seed {} step {} in {}", GetParam(), step,
                                 toString(exchange.phase())));
        if (below(30) == 0) {
            constexpr std::array kPhases{Phase::Continuous, Phase::OpeningAuction,
                                         Phase::HaltAuction, Phase::ClosingAuction,
                                         Phase::Closed};
            const Phase before = exchange.phase();
            events.clear();
            exchange.setPhase(kPhases[below(kPhases.size())], events);
            ASSERT_NO_FATAL_FAILURE(take(events));
            if (isAuction(before) && !isAuction(exchange.phase())) {
                ++uncrosses;
                ASSERT_EQ(exchange.book().indicative(exchange.reference()), std::nullopt)
                    << "the book is still crossed after the uncross";
            }
            continue;
        }

        const AgentId agent =
            below(50) == 0 ? kUnknownAgent : static_cast<AgentId>(1 + below(kConfigs.size()));
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
            NewOrder order{.clientOrderId = nextClientOrderId[agent]++,
                           .side = below(2) == 0 ? Side::Buy : Side::Sell};
            const std::uint64_t kind = below(10);
            if (kind < 2) {
                order.timeInForce = TimeInForce::ImmediateOrCancel;
            } else if (kind < 4) {
                order.type = OrderType::Market;
            } else if (kind < 5) {
                order.timeInForce = TimeInForce::PostOnly;
            }
            order.price = between(95, 105);
            order.quantity = between(1, 30);
            request = order;
        } else if (action < 80) {
            request = CancelOrder{.clientOrderId = pickClientOrderId()};
        } else {
            request = ModifyOrder{.clientOrderId = pickClientOrderId(),
                                  .price = between(95, 105),
                                  .quantity = between(1, 30)};
        }

        const Phase phase = exchange.phase();
        events.clear();
        exchange.handle(agent, request, events);
        ASSERT_FALSE(events.empty());
        ASSERT_EQ(recipient(events.front()), agent);
        if (agent != kUnknownAgent) {
            const auto* rejected = std::get_if<OrderRejected>(&events.front());
            const auto* order = std::get_if<NewOrder>(&request);
            if (phase == Phase::Closed && !std::holds_alternative<CancelOrder>(request)) {
                ASSERT_TRUE(rejected != nullptr);
                ASSERT_EQ(rejected->reason, RejectReason::MarketClosed);
            } else if (isAuction(phase) && order != nullptr &&
                       (order->type == OrderType::Market ||
                        order->timeInForce != TimeInForce::GoodTillCancel)) {
                ASSERT_TRUE(rejected != nullptr);
                ASSERT_EQ(rejected->reason, RejectReason::AuctionOrderType);
            }
        }
        ASSERT_NO_FATAL_FAILURE(take(events));
    }
    EXPECT_GT(uncrosses, 5);
    EXPECT_GT(halts, 0);
}

INSTANTIATE_TEST_SUITE_P(Seeds, TradingDayRandomTest, ::testing::Range<std::uint64_t>(1, 31));

} // namespace
} // namespace crowdbook
