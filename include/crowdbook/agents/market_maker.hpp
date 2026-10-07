#pragma once

#include <cstdint>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
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
};

struct Quotes {
    std::optional<Price> bid{};
    std::optional<Price> ask{};

    friend bool operator==(const Quotes&, const Quotes&) = default;
};

// The Avellaneda–Stoikov quotes for a fair price s and inventory q, in whole ticks:
//   reservation price  r = s - q * gamma * sigma^2 * tau
//   half spread        d = gamma * sigma^2 * tau / 2 + ln(1 + gamma / k) / gamma
// with bid = floor(r - d) and ask = ceil(r + d). A side is left out when filling it would take
// |q| past maxInventory.
[[nodiscard]] Quotes avellanedaStoikovQuotes(const MarketMakerConfig& config, double fairPrice,
                                             Quantity inventory);

// A market maker after Avellaneda and Stoikov (2008). It keeps one bid and one ask around a
// reservation price that leans against its inventory: when long it lowers both quotes, so it sells
// more and buys less, and when short it raises them. The fair price is a running average of trade
// prices (the mid before the first trade), not the mid itself, because the mid is often the market
// maker's own quotes. It requotes on a timer and straight after each of its own fills, moving a
// live quote with modify.
class MarketMaker final : public Agent {
public:
    // Throws std::invalid_argument for a config the formulas cannot use.
    MarketMaker(const MarketMakerConfig& config, Price referencePrice);

    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;
    void onFilled(AgentContext& context, const OrderFilled& event) override;
    void onTopOfBook(AgentContext& context, const TopOfBook& top) override;
    void onTrade(AgentContext& context, const Trade& trade) override;

    [[nodiscard]] double fairPrice() const noexcept;

private:
    // A quote the market maker has asked for: its client order id and the last price requested.
    struct Quote {
        ClientOrderId clientOrderId = 0;
        Price price = 0;
    };

    void requote(AgentContext& context);
    void maintain(AgentContext& context, std::optional<Quote>& quote, Side side,
                  std::optional<Price> price) const;

    MarketMakerConfig config_;
    MarketView market_;
    std::optional<double> tradeAverage_;
    std::optional<Quote> bid_;
    std::optional<Quote> ask_;
};

} // namespace crowdbook
