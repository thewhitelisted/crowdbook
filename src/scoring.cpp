#include "crowdbook/scoring.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <variant>

namespace crowdbook {

namespace {

// Rounds to whole points; the parts are far inside the range of int64.
Points points(double value) {
    return static_cast<Points>(std::llround(value));
}

double sign(Side side) {
    return side == Side::Buy ? 1.0 : -1.0;
}

} // namespace

Scorer::Scorer(ScoringConfig config, Price referencePrice)
    : config_(config), referencePrice_(referencePrice) {
    if (config_.inventoryPenalty < 0 || config_.closePenalty < 0 || config_.maxLoss < 0) {
        throw std::invalid_argument("scoring penalties and the loss limit must not be negative");
    }
    if (config_.target &&
        (config_.target->quantity <= 0 || config_.target->unfinishedPenalty < 0)) {
        throw std::invalid_argument(
            "a target needs a positive quantity and an unfinished penalty that is not negative");
    }
}

void Scorer::addSeat(AgentId agent, Cash initialCash, Quantity initialPosition) {
    seats_.insert_or_assign(agent, Seat{.initialCash = initialCash,
                                        .initialPosition = initialPosition,
                                        .cash = initialCash,
                                        .position = initialPosition});
    order_.push_back(agent);
}

void Scorer::onRequest(Timestamp /*time*/, AgentId /*agent*/, const Request& /*request*/) {}

void Scorer::onEvent(Timestamp time, const Event& event) {
    if (const auto* fill = std::get_if<OrderFilled>(&event)) {
        const auto found = seats_.find(fill->agent);
        if (found == seats_.end()) {
            return;
        }
        Seat& seat = found->second;
        seat.lotNanoseconds += static_cast<double>(std::abs(seat.position)) *
                               static_cast<double>(time - seat.since);
        seat.since = time;
        const Cash notional = fill->price * fill->quantity;
        if (fill->side == Side::Buy) {
            seat.position += fill->quantity;
            seat.cash -= notional;
        } else {
            seat.position -= fill->quantity;
            seat.cash += notional;
        }
        seat.fees += fill->fee;
    } else if (const auto* trade = std::get_if<Trade>(&event)) {
        lastPrice_ = trade->price;
        tradedValue_ += trade->price * trade->quantity;
        tradedVolume_ += trade->quantity;
        if (config_.maxLoss == 0) {
            return;
        }
        // The fills of this trade came just before it, so every seat's balances are up to date.
        for (const AgentId agent : order_) {
            Seat& seat = seats_.at(agent);
            if (seat.stoppedAt) {
                continue;
            }
            const double pnl =
                static_cast<double>(seat.cash - seat.initialCash) +
                static_cast<double>(seat.position - seat.initialPosition) *
                    static_cast<double>(trade->price) -
                static_cast<double>(seat.fees) / static_cast<double>(kPointsPerTickLot);
            if (pnl <= -static_cast<double>(config_.maxLoss)) {
                seat.stoppedAt = time;
            }
        }
    }
}

std::optional<Timestamp> Scorer::stoppedAt(AgentId agent) const {
    return seats_.at(agent).stoppedAt;
}

Score Scorer::score(AgentId agent, Timestamp end, std::optional<double> value) const {
    const Seat& seat = seats_.at(agent);
    if (config_.mark == Mark::Value && !value) {
        throw std::invalid_argument("scoring at the true value needs a fundamental value");
    }
    const double mark = config_.mark == Mark::Value
                            ? *value
                            : static_cast<double>(lastPrice_.value_or(referencePrice_));
    const double perTickLot = static_cast<double>(kPointsPerTickLot);
    Score score;
    score.pnl = points((static_cast<double>(seat.cash - seat.initialCash) +
                        static_cast<double>(seat.position - seat.initialPosition) * mark) *
                           perTickLot -
                       static_cast<double>(seat.fees));
    const double lotSeconds =
        (seat.lotNanoseconds + static_cast<double>(std::abs(seat.position)) *
                                   static_cast<double>(end - seat.since)) /
        static_cast<double>(kSecond);
    score.inventory = points(static_cast<double>(config_.inventoryPenalty) * lotSeconds);
    score.close = config_.closePenalty * std::abs(seat.position);
    if (const auto& target = config_.target) {
        const double benchmark =
            target->benchmark == Benchmark::Vwap && tradedVolume_ > 0
                ? static_cast<double>(tradedValue_) / static_cast<double>(tradedVolume_)
                : static_cast<double>(referencePrice_);
        score.paper = points(sign(target->side) * static_cast<double>(target->quantity) *
                             (mark - benchmark) * perTickLot);
        const Quantity done = target->side == Side::Buy
                                  ? seat.position - seat.initialPosition
                                  : seat.initialPosition - seat.position;
        score.unfinishedLots = std::max<Quantity>(0, target->quantity - done);
        score.unfinished = target->unfinishedPenalty * score.unfinishedLots;
    }
    score.total = score.pnl - score.inventory - score.close - score.paper - score.unfinished;
    score.stoppedAt = seat.stoppedAt;
    return score;
}

std::string_view toString(Mark mark) noexcept {
    switch (mark) {
    case Mark::LastTrade:
        return "last";
    case Mark::Value:
        return "value";
    }
    return "unknown";
}

std::string_view toString(Benchmark benchmark) noexcept {
    switch (benchmark) {
    case Benchmark::Vwap:
        return "vwap";
    case Benchmark::Reference:
        return "reference";
    }
    return "unknown";
}

} // namespace crowdbook
