#pragma once

#include <cstdint>

#include "crowdbook/agent.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// How an execution agent spreads a parent order over time.
enum class ExecutionStyle : std::uint8_t {
    Twap, // a child order of childSize every interval: an even pace in time
    Pov,  // every interval, enough to keep its trading at `participation` of all the volume
};

struct ExecutionConfig {
    ExecutionStyle style = ExecutionStyle::Twap;
    // Parent sizes in lots follow a Pareto distribution, P(size > q) = (minParent / q)^parentTail
    // for q of at least minParent, cut off at maxParent.
    Quantity minParent = 20;
    double parentTail = 1.5;
    Quantity maxParent = 5'000;
    Duration pause = 300 * kSecond; // mean of the exponential wait before each parent
    Duration interval = kSecond;    // between child orders
    Quantity childSize = 5;         // lots per child order; for POV, the most per child
    double participation = 0.1;     // POV: its share of all the volume while it works a parent
};

// A parent size drawn from the distribution ExecutionConfig describes.
[[nodiscard]] Quantity drawParentSize(Random& random, Quantity minParent, double parentTail,
                                      Quantity maxParent);

// A broker's execution algorithm working large orders for clients, one at a time. After an
// exponential pause it takes a parent order of a random side and a Pareto-distributed size, then
// trades it with child market orders, each labelled with the parent's id (1, 2, ... for this
// agent) so that an analysis of the event log can put a parent back together. TWAP sends a child
// of childSize every interval; POV checks the market's volume every interval and sends what keeps
// its own lots at `participation` of all those traded since the parent started, up to childSize
// at a time. Lots a child leaves unfilled, when the book runs out, are sent again. If the exchange
// rejects a child, such as at the account's position limit, the agent gives up on the rest of the
// parent. The next pause starts once every child of the parent has been resolved.
//
// With parent sizes this heavy-tailed, the signs of the market orders a crowd of these agents
// sends stay correlated over long stretches: a long parent keeps sending the same sign. Lillo,
// Mike and Farmer (2005) derived that the correlation then decays as a power of the lag, with
// exponent parentTail - 1.
class ExecutionTrader final : public Agent {
public:
    // Throws std::invalid_argument unless 1 <= minParent <= maxParent <= kMaxQuantity, parentTail
    // is positive and finite, the pause is not negative, the interval is positive,
    // 1 <= childSize <= kMaxQuantity and participation is strictly between 0 and 1.
    explicit ExecutionTrader(const ExecutionConfig& config);

    // It looks at the market only when it acts, so it reads snapshots.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;
    void onRejected(AgentContext& context, const OrderRejected& event) override;
    void onCancelled(AgentContext& context, const OrderCancelled& event) override;

    // The parent being worked, or 0 between parents; its side and size; and its lots not yet sent.
    [[nodiscard]] std::uint64_t parent() const noexcept { return parent_; }
    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] Quantity parentSize() const noexcept { return size_; }
    [[nodiscard]] Quantity unsent() const noexcept { return unsent_; }

private:
    void pauseThenStart(AgentContext& context) const;
    void start(AgentContext& context);
    // The lots to send now: TWAP's even pace, or what POV's share of the volume allows.
    [[nodiscard]] Quantity due(AgentContext& context) const;

    ExecutionConfig config_;
    std::uint64_t parent_ = 0;
    std::uint64_t parentsStarted_ = 0;
    Side side_ = Side::Buy;
    Quantity size_ = 0;
    Quantity unsent_ = 0;
    bool abandoned_ = false;   // after a rejection, until the next parent
    Quantity startVolume_ = 0; // POV: the market's volume when the parent started
};

} // namespace crowdbook
