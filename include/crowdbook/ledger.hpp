#pragma once

#include <map>

#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// One of an agent's own orders, as the agent knows it.
struct OwnOrder {
    ClientOrderId clientOrderId = 0;
    OrderId orderId = 0; // 0 until the exchange acknowledges the order
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;
    Quantity leaves = 0; // open quantity as last reported; the full size until acknowledged
    bool acknowledged = false;
    bool cancelRequested = false;

    friend bool operator==(const OwnOrder&, const OwnOrder&) = default;
};

// An agent's view of its own cash, position, fees and orders, built only from the requests it
// sent and the private events it received. With latency the view lags the exchange: orders are in
// flight until acknowledged, and a cancel can cross a fill on the way.
class Ledger {
public:
    explicit Ledger(Cash cash = 0, Quantity position = 0) noexcept;

    // Records a request the agent has just sent. Throws std::logic_error if a new order reuses
    // the client order id of an order that is still open or in flight.
    void recordRequest(const Request& request);
    // Applies an event addressed to this agent; public market data is ignored. Throws
    // std::logic_error for an event about an order the agent never sent.
    void apply(const Event& event);

    [[nodiscard]] Cash cash() const noexcept { return cash_; }
    [[nodiscard]] Quantity position() const noexcept { return position_; }
    // Fees paid, net of rebates, in fee units.
    [[nodiscard]] Fee fees() const noexcept { return fees_; }
    // Orders that are open or in flight, by client order id.
    [[nodiscard]] const std::map<ClientOrderId, OwnOrder>& orders() const noexcept {
        return orders_;
    }
    // The order with this client order id, or nullptr if it is not open or in flight.
    [[nodiscard]] const OwnOrder* find(ClientOrderId clientOrderId) const;
    // Open quantity on one side, including orders still in flight.
    [[nodiscard]] Quantity openQuantity(Side side) const noexcept;

private:
    OwnOrder& known(ClientOrderId clientOrderId);
    void forget(ClientOrderId clientOrderId);

    Cash cash_ = 0;
    Quantity position_ = 0;
    Fee fees_ = 0;
    std::map<ClientOrderId, OwnOrder> orders_;
};

} // namespace crowdbook
