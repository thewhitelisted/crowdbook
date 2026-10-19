#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/belief.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// The defaults suit prices of a few thousand ticks that move a few ticks a second. Keep
// gamma * sigma^2 * tau, the quote skew per lot of inventory, small: when other traders anchor on
// the market maker's quotes, a large skew drags the whole market against its own inventory.
struct MarketMakerConfig {
    double riskAversion = 0.005; // gamma: how hard inventory pushes the quotes, per tick
    double volatility = 3.0;     // sigma: ticks per square root of a second
    double intensity = 0.3;      // k: how fast fills dry up away from the fair price, per tick
    Duration horizon = kSecond;  // tau: the holding period inventory risk is priced over
    Quantity quoteSize = 5;
    Quantity maxInventory = 50; // never quote a side that could take |position| past this
    Duration requoteInterval = 100 * kMillisecond;
    double fairValueWeight = 0.2; // how far each trade moves the fair price toward its price
    // Quote with post-only orders, kept a tick off the far side of the book as last seen, so the
    // market maker only ever adds liquidity (and earns any maker rebate).
    bool postOnly = false;
    // With a belief, the fair price is the value as the belief sees it, looked at on every
    // requote, rather than the average of trades: a market maker who prices off what the asset is
    // worth.
    std::optional<BeliefConfig> valueBelief{};
    // A quiet stretch of the run, from quietFrom until quietUntil, as makers who stay away from
    // an event in play: with no quietInterval it pulls its quotes and stays out; with one it
    // requotes only that often, leaving its quotes where they are between.
    std::optional<Timestamp> quietFrom{};
    Timestamp quietUntil = kLatestTime;
    Duration quietInterval = 0;
    // A ladder of quotes a side instead of one: `levels` of them, each `levelSpacing` ticks
    // behind the one before, the first where the one quote would be, and each `levelGrowth` times
    // the size of the one before, so that the book is deeper further out, as real books are.
    int levels = 1;
    Price levelSpacing = 1;
    double levelGrowth = 1.0;
    // Ticks: at a requote its timer brings, it leaves its quotes where they are while its fair
    // price is less than this from where it was when it last set them and every quote it set
    // then is still in the book at its full size, as makers who do not chase each small move of
    // a reference do. A fill, or a rejected modify, requotes it whatever the threshold; a side
    // left out then, by its limits or a post-only quote the book would have met, stays out until
    // it requotes. 0 requotes on every tick of the timer.
    double requoteThreshold = 0.0;
    // Reading order flow, as in Glosten and Milgrom (1985) and Kyle (1985): each lot of
    // aggressive volume in a trade it sees moves its fair price this many ticks, up for a buy and
    // down for a sell, on top of the price it anchors on. Auction trades, which have no aggressor,
    // move it not at all. 0 reads no flow.
    double flowImpact = 0.0;
    // How long that impact lasts: it decays by a factor of e every flowMemory. None keeps it for
    // good, as flow from traders who know the value; a finite memory makes the mid revert.
    std::optional<Duration> flowMemory{};
    // Ticks: at a requote, when its fair price is past the far side of the book by more than
    // this, it sends an immediate-or-cancel order against the stale quotes there, at its fair
    // price less this, for as much as max_inventory leaves room for, counting every order it has
    // open, before it quotes. None never takes.
    std::optional<double> takeThrough{};
};

struct Quotes {
    std::optional<Price> bid{};
    std::optional<Price> ask{};

    friend bool operator==(const Quotes&, const Quotes&) = default;
};

// The Avellaneda–Stoikov quotes for a fair price s and inventory q, in whole ticks:
//   reservation price  r = s - q * gamma * sigma^2 * tau
//   half spread        d = gamma * sigma^2 * tau / 2 + ln(1 + gamma / k) / gamma
// with bid = floor(r - d) and ask = ceil(r + d), each kept within 1 to kMaxPrice. A side is left
// out when filling it would take |q| past maxInventory, and the bid when it is below 1.
[[nodiscard]] Quotes avellanedaStoikovQuotes(const MarketMakerConfig& config, double fairPrice,
                                             Quantity inventory);

// A market maker after Avellaneda and Stoikov (2008). It keeps one bid and one ask, or a ladder of
// them, around a
// reservation price that leans against its inventory: when long it lowers both quotes, so it sells
// more and buys less, and when short it raises them. The fair price is a running average of trade
// prices (the mid before the first trade), not the mid itself, because the mid is often the market
// maker's own quotes; or, with a value belief, the value as it sees it at each requote. It quotes
// on start, then requotes on a timer (whose first tick falls at a random point in the first
// interval) and straight after each of its own fills, moving a live quote with modify. A modify the
// exchange rejects is sent again at the next requote.
class MarketMaker final : public Agent {
public:
    // Throws std::invalid_argument for a config the formulas cannot use, and for a value belief
    // without a value or one Belief refuses.
    MarketMaker(const MarketMakerConfig& config, Price referencePrice,
                std::shared_ptr<Fundamental> fundamental = nullptr);

    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;
    void onFilled(AgentContext& context, const OrderFilled& event) override;
    void onRejected(AgentContext& context, const OrderRejected& event) override;
    // Its quotes stand through an auction, but it sends no new ones until trading is
    // continuous again.
    void onPhase(AgentContext& context, const PhaseChanged& phase) override;
    void onTopOfBook(AgentContext& context, const TopOfBook& top) override;
    void onTrade(AgentContext& context, const Trade& trade) override;

    // The running average of trades; a market maker with a value belief quotes around the value
    // instead.
    [[nodiscard]] double fairPrice() const noexcept;
    // What the order flow it has seen adds to its fair price at `now`, in ticks.
    [[nodiscard]] double flowImpact(Timestamp now) const noexcept;

private:
    Phase phase_ = Phase::Continuous;
    // A quote the market maker has asked for: its client order id and the last price requested.
    struct Quote {
        ClientOrderId clientOrderId = 0;
        Price price = 0;
    };

    // `afterFill` requotes whatever the threshold.
    void requote(AgentContext& context, bool afterFill = false);
    // Whether every quote it set at its last requote is in the book at its full size, none
    // filled or on its way out.
    [[nodiscard]] bool standing(const AgentContext& context) const;
    void maintain(AgentContext& context, std::optional<Quote>& quote, Side side,
                  std::optional<Price> price, Quantity size) const;
    // Lots quoted at each level of the ladder, the touch first.
    [[nodiscard]] Quantity levelSize(int level) const noexcept;
    // With takeThrough, trades against quotes its fair price has moved past; returns the
    // position it would hold were every take still open to fill.
    Quantity takeThrough(AgentContext& context, double fair);

    MarketMakerConfig config_;
    MarketView market_;
    std::optional<double> tradeAverage_;
    std::optional<Belief> belief_;
    std::optional<Timestamp> quotedAt_; // its last requote in a quiet stretch
    std::optional<double> quotedFair_;  // its fair price when it last set its quotes
    double impact_ = 0.0;               // the flow's impact on its fair price, as of impactAt_
    Timestamp impactAt_ = 0;
    std::vector<ClientOrderId> takes_;  // its take-through orders, until they are done
    // Its quotes a side, a level each, the touch first.
    std::vector<std::optional<Quote>> bids_;
    std::vector<std::optional<Quote>> asks_;
};

} // namespace crowdbook
