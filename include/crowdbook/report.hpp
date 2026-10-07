#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/scoring.hpp"
#include "crowdbook/session.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// How long after a fill the report looks at the mid again.
inline constexpr std::array<Duration, 3> kMarkoutHorizons{kSecond, 10 * kSecond, 60 * kSecond};

// One of a seat's fills, with who was on the other side and what the market did next.
struct ReportFill {
    Timestamp time = 0;
    Side side = Side::Buy; // the seat's side
    Price price = 0;
    Quantity quantity = 0;
    Liquidity liquidity = Liquidity::Maker; // the seat's
    Fee fee = 0;
    AgentId counterparty = 0;
    std::string group{}; // the counterparty's group and agent type
    std::string type{};
    std::optional<double> value{}; // the true value at the fill, with a fundamental value
    // The mid each of kMarkoutHorizons later, if the session lasted that long and the book had
    // both sides then.
    std::array<std::optional<double>, kMarkoutHorizons.size()> midAfter{};
};

// What a seat's trading with one group came to.
struct Counterparty {
    std::string group{};
    std::string type{};
    std::int64_t fills = 0;
    Quantity bought = 0; // by the seat, from this group
    Quantity sold = 0;
    // The group's edge against the seat at the true value, in ticks per lot: how far the price
    // was from the value in the group's favour. Without a fundamental value, nullopt.
    std::optional<double> edge{};
    // The seat's markout ten seconds after its fills with this group, in ticks per lot: the mid
    // then against the price, positive when it went the seat's way.
    std::optional<double> markout{};
};

// A seat's balances at one moment.
struct PnlPoint {
    Timestamp time = 0;
    Quantity position = 0;
    Cash cash = 0;
    Fee fees = 0;
    double pnl = 0.0;              // tick-lots, net of fees, positions at the last trade price
    std::optional<double> pnlAtValue{}; // and at the true value, with a fundamental value
};

// The mid in the session and in the session replayed without the seat.
struct PricePoint {
    Timestamp time = 0;
    std::optional<double> mid{};
    std::optional<double> midWithout{};
};

struct SeatReport {
    std::string seat{};
    std::optional<Score> score{};
    std::vector<ReportFill> fills{};
    std::vector<Counterparty> counterparties{}; // most lots first
    std::vector<PnlPoint> pnl{};
    std::vector<PricePoint> prices{};
    Price lastPrice = 0; // the last trade price, with the seat and without it
    Price lastPriceWithout = 0;
    std::optional<double> vwap{}; // of every trade, with the seat and without it
    std::optional<double> vwapWithout{};
    // The mid with the seat less the mid without it, averaged over the moments both had one.
    std::optional<double> impact{};
};

struct SessionReport {
    std::uint64_t seed = 0;
    Timestamp end = 0;
    std::optional<double> finalValue{};
    std::vector<SeatReport> seats{};
};

// Replays a session, and again without each seat, and reports on every seat, with balances and
// prices every `interval`. Throws ScenarioError and std::logic_error as replaySession does, and
// std::invalid_argument unless the interval is positive.
[[nodiscard]] SessionReport makeReport(const Session& session, const AgentRegistry& registry,
                                       Duration interval = kSecond);

// Writes the report as one JSON object; docs/scenarios.md describes it.
void writeReportJson(std::ostream& out, const SessionReport& report);

} // namespace crowdbook
