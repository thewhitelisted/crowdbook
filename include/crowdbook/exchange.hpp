#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdbook/messages.hpp"
#include "crowdbook/order_book.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// The deepest depth feed an exchange can publish, in price levels per side.
inline constexpr std::size_t kMaxDepthLevels = 1'000;

// Settings that apply to everyone trading on the exchange.
struct ExchangeConfig {
    // Price levels per side in the public depth feed; 0 publishes no depth, only the best bid
    // and ask.
    std::size_t depthLevels = 0;
    // Fees per lot, in fee units, charged on every fill: the owner of the resting order pays
    // makerFee and the owner of the incoming order takerFee. A negative fee is a rebate.
    Fee makerFee = 0;
    Fee takerFee = 0;
};

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
    Fee fees = 0;                  // paid to the exchange; negative when rebates exceed fees

    // Cash plus the position valued at `markPrice`, before fees.
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
    // Throws std::invalid_argument for a fee rate beyond kMaxFeeRate, fees that would pay out
    // more in rebates than they collect on a trade, or a depth feed deeper than kMaxDepthLevels.
    explicit Exchange(const ExchangeConfig& config = {});

    // Opens an account. Throws std::invalid_argument if the agent already has one or a limit is out
    // of range.
    void addAgent(AgentId agent, const AccountConfig& config = {});

    // Processes one request and appends its events in this order: the requesting agent's
    // acceptance, modification, cancellation or rejection; then, for each execution, the maker's
    // fill, the taker's fill and the public trade; then the cancellation of any quantity that could
    // not rest; then a top-of-book update if the best bid or ask changed; and last, with a depth
    // feed, a depth update if any published level changed.
    void handle(AgentId agent, const Request& request, std::vector<Event>& events);

    // Throws std::out_of_range for an agent without an account.
    [[nodiscard]] const Account& account(AgentId agent) const;
    [[nodiscard]] std::optional<OrderId> liveOrderId(AgentId agent,
                                                     ClientOrderId clientOrderId) const;
    [[nodiscard]] TopOfBook topOfBook() const;
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const ExchangeConfig& config() const noexcept { return config_; }
    // Fees collected from every agent, net of rebates, in fee units.
    [[nodiscard]] Fee feesCollected() const noexcept { return feesCollected_; }

    // Cross-checks accounts, live orders and the book, including conservation of cash, shares
    // and fees, and describes the first inconsistency found. Walks everything, so it is meant for
    // tests.
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
        bool postOnly = false;
    };

    // An order that has just been through matching: new, or re-entered by a modify.
    struct IncomingOrder {
        AgentId agent = 0;
        ClientOrderId clientOrderId = 0;
        OrderId id = 0;
        Side side = Side::Buy;
        Quantity quantity = 0;      // quantity sent to the book
        Quantity restingBefore = 0; // open quantity it had on the book beforehand
        bool postOnly = false;
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
    void publishDepth(std::vector<Event>& events);
    // Whether a limit order on `side` at `price` would trade against the book now.
    [[nodiscard]] bool wouldTrade(Side side, Price price) const noexcept;

    ExchangeConfig config_;
    OrderBook book_;
    std::unordered_map<AgentId, AgentState> agents_;
    std::unordered_map<OrderId, LiveOrder> liveOrders_;
    OrderId nextOrderId_ = 1;
    TopOfBook publishedTop_;
    BookDepth publishedDepth_;
    BookDepth currentDepth_; // reused for every request
    Fee feesCollected_ = 0;
    std::vector<Fill> fills_; // reused for every request
};

} // namespace crowdbook
