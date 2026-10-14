#include "crowdbook/learning_config.hpp"

namespace crowdbook {

namespace {

constexpr std::array<std::string_view, kLearningActions.size()> kActionNames{
    "hold",         "bid_join",   "ask_join",   "both_join",  "bid_improve", "ask_improve",
    "both_improve", "bid_behind", "ask_behind", "both_behind", "cancel_bid", "cancel_ask",
    "cancel_all",   "buy",        "sell",       "close"};

} // namespace

std::string_view toString(LearningAction action) noexcept {
    const auto index = static_cast<std::size_t>(action);
    return index < kActionNames.size() ? kActionNames[index] : "unknown";
}

std::optional<LearningAction> learningActionNamed(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kActionNames.size(); ++i) {
        if (kActionNames[i] == name) {
            return kLearningActions[i];
        }
    }
    return std::nullopt;
}

std::string SettingRange::path() const {
    std::string joined;
    for (const std::string& key : keys) {
        if (!joined.empty()) {
            joined += '.';
        }
        joined += key;
    }
    return joined;
}

} // namespace crowdbook
