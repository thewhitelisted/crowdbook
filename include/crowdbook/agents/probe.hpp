#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// How a probe trades.
enum class ProbeStyle : std::uint8_t {
    Maker,    // quotes a bid and an ask `offset` ticks either side of the mid
    Trend,    // takes with the trend: a fast moving average of the mid above a slow one buys
    Reversal, // takes against it
    Flow,     // takes with the takers: buys when most of the recent volume was bought
};

struct ProbeConfig {
    ProbeStyle style = ProbeStyle::Maker;
    Duration interval = kSecond; // how often it looks
    Quantity size = 1;           // lots per order
    Quantity maxPosition = 10;
    double offset = 1.0;                  // a maker's half width, in ticks from the mid
    Duration fastHalfLife = 10 * kSecond; // a trend's or a reversal's averages
    Duration slowHalfLife = 60 * kSecond;
    double threshold = 1.0;         // ticks the fast average must lead the slow one by
    Duration window = 20 * kSecond; // the recent volume a flow follower weighs
    double imbalance = 0.6;         // its share on one side before it follows
};

// A small, simple strategy that measures a market rather than moves it: a maker at a set width,
// a trend follower, a reversal trader or a flow follower. Run a few of them, each alone and small,
// in a market fitted to a real one, and any that makes money after fees is an edge the fitted
// market offers; where the real market offers none, the fit is wrong. Takers take only at the
// touch, with immediate-or-cancel orders, and every probe keeps its position within
// ±maxPosition even if all its orders in flight fill.
class Probe final : public Agent {
public:
    // Throws std::invalid_argument for a non-positive interval, size or half-life, a fast half
    // life not below the slow one, a negative offset or threshold, an imbalance outside [0.5, 1)
    // or a size above the position limit.
    Probe(const ProbeConfig& config, Price referencePrice);

    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;
    void onTopOfBook(AgentContext& context, const TopOfBook& top) override;
    void onTrade(AgentContext& context, const Trade& trade) override;
    void onFilled(AgentContext& context, const OrderFilled& event) override;
    void onCancelled(AgentContext& context, const OrderCancelled& event) override;
    void onRejected(AgentContext& context, const OrderRejected& event) override;

private:
    struct Quote {
        ClientOrderId id = 0;
        Price price = 0;
    };

    void quote(AgentContext& context, Side side, Price price);
    void takeToward(AgentContext& context, Quantity target);
    void forget(ClientOrderId id);

    ProbeConfig config_;
    MarketView market_;
    double fastWeight_ = 0.0;
    double slowWeight_ = 0.0;
    std::optional<double> fast_;
    std::optional<double> slow_;
    Quantity target_ = 0;
    std::optional<Quote> bid_;
    std::optional<Quote> ask_;
    std::deque<std::pair<Timestamp, Quantity>> flow_; // recent trades, signed by the taker's side
};

} // namespace crowdbook
