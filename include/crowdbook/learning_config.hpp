#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/parameters.hpp"
#include "crowdbook/scoring.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// What the learning seat can do at a decision. Prices are relative to the best prices the seat
// sees, its own orders left out, and the seat keeps at most one resting order a side: an action
// on a side moves that order there, or leaves it alone, keeping its place in the queue, if it is
// there already.
enum class LearningAction : std::uint8_t {
    Hold,        // nothing
    BidJoin,     // a bid at the best bid
    AskJoin,     // an offer at the best offer
    BothJoin,    // both
    BidImprove,  // a bid a tick above the best bid, short of the best offer
    AskImprove,  // an offer a tick below the best offer, short of the best bid
    BothImprove, // both
    BidBehind,   // a bid a tick below the best bid
    AskBehind,   // an offer a tick above the best offer
    BothBehind,  // both
    CancelBid,   // cancel the seat's bid
    CancelAsk,   // cancel the seat's offer
    CancelAll,   // cancel both
    Buy,         // take the best offer, immediate-or-cancel
    Sell,        // take the best bid, immediate-or-cancel
    Close,       // cancel both and take the position off, immediate-or-cancel
};

inline constexpr std::array kLearningActions{
    LearningAction::Hold,       LearningAction::BidJoin,     LearningAction::AskJoin,
    LearningAction::BothJoin,   LearningAction::BidImprove,  LearningAction::AskImprove,
    LearningAction::BothImprove, LearningAction::BidBehind,  LearningAction::AskBehind,
    LearningAction::BothBehind, LearningAction::CancelBid,   LearningAction::CancelAsk,
    LearningAction::CancelAll,  LearningAction::Buy,         LearningAction::Sell,
    LearningAction::Close};

// The name an action has in scenario files: "hold", "bid_join", "both_improve" and so on.
[[nodiscard]] std::string_view toString(LearningAction action) noexcept;
// The action with this name, if there is one.
[[nodiscard]] std::optional<LearningAction> learningActionNamed(std::string_view name) noexcept;

// A public feed of the market's underlying, which only the learning seat hears: every interval,
// the underlying as it stood `lag` earlier, plus fresh normal noise. The underlying is the true
// value of a market with a fundamental, in ticks, and in a prediction market the hidden quantity
// whose end decides the question, whose change over the run has variance 1.
struct LearningFeed {
    Duration interval = kSecond;
    Duration lag = 0;
    double noise = 0.0; // standard deviation, in the underlying's units

    friend bool operator==(const LearningFeed&, const LearningFeed&) = default;
};

// A scenario setting drawn at random for each episode.
struct SettingRange {
    enum class Form : std::uint8_t {
        // Uniform between low and high: whole numbers and durations inclusive. With a step, one
        // of low, low + step, low + 2 step and so on up to high, each as likely, for settings
        // such as fees that take only so many decimals.
        Uniform,
        Log,     // uniform in the logarithm, between positive low and high
        Choices, // one of the choices, each as likely
    };

    std::vector<std::string> keys{}; // where the setting is, as in the scenario file
    Form form = Form::Uniform;
    // Both whole numbers, both numbers, or both durations written as text.
    Parameters::Value low{};
    Parameters::Value high{};
    std::optional<Parameters::Value> step{}; // of the same type as low and high
    std::vector<Parameters::Value> choices{};

    // The keys joined with dots, as in error messages.
    [[nodiscard]] std::string path() const;

    friend bool operator==(const SettingRange&, const SettingRange&) = default;
};

// How a market is stepped by a learning agent: its [learning] section. Agents' runs ignore it.
struct LearningConfig {
    // A decision is due at every multiple of `interval` from the start, and whenever one of the
    // seat's own orders fills; the first at `warmup`.
    Duration interval = kSecond;
    Duration warmup = kSecond;
    // The actions, in the order an agent numbers them; every action by default.
    std::vector<LearningAction> actions{kLearningActions.begin(), kLearningActions.end()};
    // The order sizes an action can be taken at, in lots, cut to what the position limit and the
    // largest order allow.
    std::vector<Quantity> sizes{1, 5};
    Price takeTicks = 0; // how far past the best price a take or a close may trade
    // What positions are valued at between decisions, for the reward, and at the end. Unset,
    // rewards value them at the true value where the market has one and through the book where
    // it has none, and the end values them at resolution in a prediction market and through the
    // book in any other.
    std::optional<Mark> rewardMark{};
    std::optional<Mark> close{};
    // The observation's capacity: events since the last decision, depth updates among them,
    // levels a side in each, and the seat's open orders.
    std::size_t maxEvents = 256;
    std::size_t maxDepthUpdates = 32;
    std::size_t depthLevels = 10;
    std::size_t maxOrders = 8;
    std::optional<LearningFeed> feed{};
    std::vector<SettingRange> ranges{};

    friend bool operator==(const LearningConfig&, const LearningConfig&) = default;
};

} // namespace crowdbook
