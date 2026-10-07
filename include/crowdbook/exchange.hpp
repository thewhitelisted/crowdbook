#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdbook/messages.hpp"
#include "crowdbook/order_book.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct AccountConfig {
    Cash initialCash = 0;
    Quantity initialPosition = 0;
    // Largest absolute position allowed, checked as if every open order on a side filled.
    Quantity maxPosition = kMaxQuantity;
    Quantity maxOrderQuantity = kMaxQuantity;
};

// An agent's balances as the exchange keeps them. Cash may go negative: risk is bounded by the
// position limit alone.
struct Account {
    Cash cash = 0;
    Quantity position = 0;         // negative when short
    Quantity openBuyQuantity = 0;  // resting buy quantity
    Quantity openSellQuantity = 0; // resting sell quantity

    // Cash plus the position valued at `markPrice`.
    [[nodiscard]] constexpr Cash equity(Price markPrice) const noexcept {
        return cash + position * markPrice;
    }

    friend bool operator==(const Account&, const Account&) = default;
};

// The only writer of the order book and of every account. Agents act by sending requests; the
// exchange validates each one, assigns order ids, enforces risk limits, settles trades and reports
// everything that happened as events.
class Exchange {
public:
    // Opens an account. Throws std::invalid_argument if the agent already has one or a limit is out
    // of range.
    void addAgent(AgentId agent, const AccountConfig& config = {});

    // Processes one request and appends its events in this order: the requesting agent's
    // acceptance, modification, cancellation or rejection; then, for each execution, the maker's
    // fill, the taker's fill and the public trade; then the cancellation of any quantity that could
    // not rest; and last a top-of-book update if the best bid or ask changed.
    void handle(AgentId agent, const Request& request, std::vector<Event>& events);

    // Throws std::out_of_range for an agent without an account.
    [[nodiscard]] const Account& account(AgentId agent) const;
    [[nodiscard]] std::optional<OrderId> liveOrderId(AgentId agent,
                                                     ClientOrderId clientOrderId) const;
    [[nodiscard]] TopOfBook topOfBook() const;
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

    // Cross-checks accounts, live orders and the book, including conservation of cash and shares,
    // and describes the first inconsistency found. Walks everything, so it is meant for tests.
    [[nodiscard]] std::optional<std::string> audit() const;

private:
    struct AgentState {
        AccountConfig config{};
        Account account{};
        std::unordered_map<ClientOrderId, OrderId> liveOrders{};
    };

    struct LiveOrder {
        AgentId agent = 0;
        ClientOrderId clientOrderId = 0;
        Side side = Side::Buy;
    };

    // An order that has just been through matching: new, or re-entered by a modify.
    struct IncomingOrder {
        AgentId agent = 0;
        ClientOrderId clientOrderId = 0;
        OrderId id = 0;
        Side side = Side::Buy;
        Quantity quantity = 0;      // quantity sent to the book
        Quantity restingBefore = 0; // open quantity it had on the book beforehand
    };

    void submit(AgentId agent, AgentState& state, const NewOrder& order,
                std::vector<Event>& events);
    void cancel(AgentId agent, AgentState& state, const CancelOrder& request,
                std::vector<Event>& events);
    void modify(AgentId agent, AgentState& state, const ModifyOrder& request,
                std::vector<Event>& events);
    // Settles the executions in fills_ and reports them.
    void settle(const IncomingOrder& incoming, std::vector<Event>& events);
    // Updates live-order records and open quantity to match the book after matching.
    void finish(const IncomingOrder& incoming, AgentState& state, const OrderResult& result,
                std::vector<Event>& events);
    void publishTopOfBook(std::vector<Event>& events);

    OrderBook book_;
    std::unordered_map<AgentId, AgentState> agents_;
    std::unordered_map<OrderId, LiveOrder> liveOrders_;
    OrderId nextOrderId_ = 1;
    TopOfBook publishedTop_;
    std::vector<Fill> fills_; // reused for every request
};

} // namespace crowdbook
