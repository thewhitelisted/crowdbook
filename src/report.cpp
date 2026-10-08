#include "crowdbook/report.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "crowdbook/fundamental.hpp"
#include "crowdbook/scenario_file.hpp"

namespace crowdbook {

namespace {

struct TradePrint {
    Timestamp time = 0;
    Price price = 0;
    Quantity quantity = 0;
};

struct MidChange {
    Timestamp time = 0;
    std::optional<double> mid{};
};

struct Balance {
    Timestamp time = 0;
    Quantity position = 0;
    Cash cash = 0;
    Fee fees = 0;
};

// Keeps what a report needs from a replay: the trades, the mid after every change of the best
// prices and, for the seats, each fill with its counterparty and the balances after it.
class Recorder final : public EventSink {
public:
    void watch(AgentId agent, Cash cash, Quantity position) {
        balances_[agent].push_back({.position = position, .cash = cash});
        fills_[agent];
    }

    void onRequest(Timestamp /*time*/, AgentId /*agent*/, const Request& /*request*/) override {}

    void onEvent(Timestamp time, const Event& event) override {
        if (const auto* fill = std::get_if<OrderFilled>(&event)) {
            // Each execution is the maker's fill, then the taker's.
            if (fill->liquidity == Liquidity::Maker) {
                maker_ = *fill;
                return;
            }
            record(time, *maker_, fill->agent);
            record(time, *fill, maker_->agent);
            maker_.reset();
        } else if (const auto* trade = std::get_if<Trade>(&event)) {
            trades_.push_back({.time = time, .price = trade->price, .quantity = trade->quantity});
        } else if (const auto* top = std::get_if<TopOfBook>(&event)) {
            std::optional<double> mid;
            if (top->bid && top->ask) {
                mid = static_cast<double>(top->bid->price + top->ask->price) / 2.0;
            }
            mids_.push_back({.time = time, .mid = mid});
        }
    }

    [[nodiscard]] const std::vector<TradePrint>& trades() const noexcept { return trades_; }
    [[nodiscard]] const std::vector<MidChange>& mids() const noexcept { return mids_; }
    [[nodiscard]] const std::vector<ReportFill>& fills(AgentId agent) const {
        return fills_.at(agent);
    }
    [[nodiscard]] const std::vector<Balance>& balances(AgentId agent) const {
        return balances_.at(agent);
    }

private:
    void record(Timestamp time, const OrderFilled& fill, AgentId counterparty) {
        const auto found = fills_.find(fill.agent);
        if (found == fills_.end()) {
            return;
        }
        found->second.push_back({.time = time,
                                 .side = fill.side,
                                 .price = fill.price,
                                 .quantity = fill.quantity,
                                 .liquidity = fill.liquidity,
                                 .fee = fill.fee,
                                 .counterparty = counterparty});
        Balance balance = balances_.at(fill.agent).back();
        balance.time = time;
        const Cash notional = fill.price * fill.quantity;
        balance.position += fill.side == Side::Buy ? fill.quantity : -fill.quantity;
        balance.cash += fill.side == Side::Buy ? -notional : notional;
        balance.fees += fill.fee;
        balances_.at(fill.agent).push_back(balance);
    }

    std::optional<OrderFilled> maker_;
    std::vector<TradePrint> trades_;
    std::vector<MidChange> mids_;
    std::unordered_map<AgentId, std::vector<ReportFill>> fills_;
    std::unordered_map<AgentId, std::vector<Balance>> balances_;
};

// The mid as of `time`: after the last change at or before it.
std::optional<double> midAt(const std::vector<MidChange>& mids, Timestamp time) {
    const auto after = std::ranges::upper_bound(mids, time, {}, &MidChange::time);
    return after == mids.begin() ? std::nullopt : std::prev(after)->mid;
}

Price lastPriceAt(const std::vector<TradePrint>& trades, Timestamp time, Price reference) {
    const auto after = std::ranges::upper_bound(trades, time, {}, &TradePrint::time);
    return after == trades.begin() ? reference : std::prev(after)->price;
}

std::optional<double> vwapOf(const std::vector<TradePrint>& trades) {
    Cash value = 0;
    Quantity volume = 0;
    for (const TradePrint& trade : trades) {
        value += trade.price * trade.quantity;
        volume += trade.quantity;
    }
    if (volume == 0) {
        return std::nullopt;
    }
    return static_cast<double>(value) / static_cast<double>(volume);
}

double sign(Side side) {
    return side == Side::Buy ? 1.0 : -1.0;
}

std::string number(const std::optional<double>& value) {
    return value ? std::format("{}", *value) : "null";
}

std::string text(std::string_view value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    return quoted + '"';
}

} // namespace

SessionReport makeReport(const Session& session, const AgentRegistry& registry,
                         Duration interval) {
    if (interval <= 0) {
        throw std::invalid_argument("the report's interval must be positive");
    }
    const Scenario scenario = sessionScenario(session);
    const AccountConfig& account = scenario.participant.account;

    // The session, watching every seat. Seats join after the scenario's agents, in order.
    Recorder recorder;
    AgentId firstSeat = 1;
    for (const AgentGroup& group : scenario.groups) {
        firstSeat += static_cast<AgentId>(group.count);
    }
    for (std::size_t i = 0; i < session.seats.size(); ++i) {
        recorder.watch(firstSeat + static_cast<AgentId>(i), account.initialCash,
                       account.initialPosition);
    }
    const RunResult result = replaySession(session, registry, &recorder);
    std::unordered_map<AgentId, const GroupResult*> groupOf;
    for (const GroupResult& group : result.groups) {
        for (const AgentId agent : group.agents) {
            groupOf[agent] = &group;
        }
    }

    // Every moment the true value is wanted, read in order from a copy of the value's path.
    std::vector<Timestamp> grid;
    for (Timestamp time = 0; time < session.end; time += interval) {
        grid.push_back(time);
    }
    grid.push_back(session.end);
    std::map<Timestamp, double> values;
    if (scenario.fundamental) {
        for (std::size_t i = 0; i < session.seats.size(); ++i) {
            for (const ReportFill& fill : recorder.fills(firstSeat + static_cast<AgentId>(i))) {
                values[fill.time] = 0.0;
            }
        }
        for (const Timestamp time : grid) {
            values[time] = 0.0;
        }
        Fundamental value{*scenario.fundamental, Random{scenario.seed, 0}};
        for (auto& [time, at] : values) {
            at = value.valueAt(time);
        }
    }
    const auto valueAt = [&](Timestamp time) -> std::optional<double> {
        const auto found = values.find(time);
        return found == values.end() ? std::nullopt : std::optional<double>{found->second};
    };

    SessionReport report{.seed = session.seed, .end = session.end, .finalValue = result.finalValue};
    for (std::uint32_t index = 0; index < session.seats.size(); ++index) {
        const AgentId agent = firstSeat + index;
        SeatReport seat{.seat = session.seats[index]};
        if (const auto score = std::ranges::find(result.scores, seat.seat, &SeatScore::seat);
            score != result.scores.end()) {
            seat.score = score->score;
        }

        // Fills, and the counterparties they add up to.
        std::map<std::string, Counterparty> byGroup;
        std::map<std::string, std::pair<double, Quantity>> edges;    // sum and lots with a value
        std::map<std::string, std::pair<double, Quantity>> markouts; // sum and lots with a mid
        for (ReportFill fill : recorder.fills(agent)) {
            const GroupResult& group = *groupOf.at(fill.counterparty);
            fill.group = group.name;
            fill.type = group.type;
            fill.value = valueAt(fill.time);
            for (std::size_t h = 0; h < kMarkoutHorizons.size(); ++h) {
                if (fill.time + kMarkoutHorizons[h] <= session.end) {
                    fill.midAfter[h] = midAt(recorder.mids(), fill.time + kMarkoutHorizons[h]);
                }
            }
            Counterparty& summary = byGroup[group.name];
            summary.group = group.name;
            summary.type = group.type;
            ++summary.fills;
            (fill.side == Side::Buy ? summary.bought : summary.sold) += fill.quantity;
            const double lots = static_cast<double>(fill.quantity);
            if (fill.value) {
                // The counterparty's side is the seat's opposite.
                auto& [sum, count] = edges[group.name];
                sum += -sign(fill.side) * (*fill.value - static_cast<double>(fill.price)) * lots;
                count += fill.quantity;
            }
            if (const auto& mid = fill.midAfter[1]) {
                auto& [sum, count] = markouts[group.name];
                sum += sign(fill.side) * (*mid - static_cast<double>(fill.price)) * lots;
                count += fill.quantity;
            }
            seat.fills.push_back(std::move(fill));
        }
        for (auto& [name, summary] : byGroup) {
            if (const auto found = edges.find(name); found != edges.end()) {
                summary.edge = found->second.first / static_cast<double>(found->second.second);
            }
            if (const auto found = markouts.find(name); found != markouts.end()) {
                summary.markout =
                    found->second.first / static_cast<double>(found->second.second);
            }
            seat.counterparties.push_back(summary);
        }
        std::ranges::stable_sort(seat.counterparties, std::greater{},
                                 [](const Counterparty& c) { return c.bought + c.sold; });

        // Balances over time.
        const std::vector<Balance>& balances = recorder.balances(agent);
        for (const Timestamp time : grid) {
            const Balance& balance =
                *std::prev(std::ranges::upper_bound(balances, time, {}, &Balance::time));
            const double held = static_cast<double>(balance.position - account.initialPosition);
            const double base = static_cast<double>(balance.cash - account.initialCash) -
                                static_cast<double>(balance.fees) /
                                    static_cast<double>(kFeeUnitsPerTickLot);
            PnlPoint point{.time = time,
                           .position = balance.position,
                           .cash = balance.cash,
                           .fees = balance.fees,
                           .pnl = base + held * static_cast<double>(lastPriceAt(
                                                    recorder.trades(), time,
                                                    scenario.referencePrice))};
            if (const auto value = valueAt(time)) {
                point.pnlAtValue = base + held * *value;
            }
            seat.pnl.push_back(point);
        }

        // The market without the seat.
        Recorder without;
        static_cast<void>(replaySession(session, registry, &without, {index}));
        double impact = 0.0;
        std::int64_t compared = 0;
        for (const Timestamp time : grid) {
            PricePoint point{.time = time,
                             .mid = midAt(recorder.mids(), time),
                             .midWithout = midAt(without.mids(), time)};
            if (point.mid && point.midWithout) {
                impact += *point.mid - *point.midWithout;
                ++compared;
            }
            seat.prices.push_back(point);
        }
        if (compared > 0) {
            seat.impact = impact / static_cast<double>(compared);
        }
        seat.lastPrice = lastPriceAt(recorder.trades(), session.end, scenario.referencePrice);
        seat.lastPriceWithout =
            lastPriceAt(without.trades(), session.end, scenario.referencePrice);
        seat.vwap = vwapOf(recorder.trades());
        seat.vwapWithout = vwapOf(without.trades());
        report.seats.push_back(std::move(seat));
    }
    return report;
}

void writeReportJson(std::ostream& out, const SessionReport& report) {
    out << std::format("{{\n  \"seed\": {},\n  \"end_ns\": {},\n  \"final_value\": {},\n",
                       report.seed, report.end, number(report.finalValue));
    out << "  \"seats\": [";
    for (std::size_t s = 0; s < report.seats.size(); ++s) {
        const SeatReport& seat = report.seats[s];
        out << (s == 0 ? "\n" : ",\n") << "    {\n";
        out << std::format("      \"seat\": {},\n", text(seat.seat));
        if (seat.score) {
            const Score& score = *seat.score;
            out << std::format("      \"score\": {{\"total\": {}, \"pnl\": {}, \"inventory\": {}, "
                               "\"close\": {}, \"paper\": {}, \"unfinished\": {}, "
                               "\"unfinished_lots\": {}, \"stopped_at_ns\": {}}},\n",
                               score.total, score.pnl, score.inventory, score.close, score.paper,
                               score.unfinished, score.unfinishedLots,
                               score.stoppedAt ? std::format("{}", *score.stoppedAt) : "null");
        } else {
            out << "      \"score\": null,\n";
        }
        out << "      \"counterparties\": [";
        for (std::size_t c = 0; c < seat.counterparties.size(); ++c) {
            const Counterparty& party = seat.counterparties[c];
            out << (c == 0 ? "\n" : ",\n");
            out << std::format("        {{\"group\": {}, \"type\": {}, \"fills\": {}, "
                               "\"bought\": {}, \"sold\": {}, \"edge\": {}, \"markout_10s\": {}}}",
                               text(party.group), text(party.type), party.fills, party.bought,
                               party.sold, number(party.edge), number(party.markout));
        }
        out << "\n      ],\n      \"fills\": [";
        for (std::size_t f = 0; f < seat.fills.size(); ++f) {
            const ReportFill& fill = seat.fills[f];
            out << (f == 0 ? "\n" : ",\n");
            out << std::format("        {{\"time_ns\": {}, \"side\": {}, \"price\": {}, "
                               "\"quantity\": {}, \"liquidity\": {}, \"fee\": {}, "
                               "\"counterparty\": {}, \"group\": {}, \"type\": {}, \"value\": {}, "
                               "\"mid_1s\": {}, \"mid_10s\": {}, \"mid_60s\": {}}}",
                               fill.time, text(toString(fill.side)), fill.price, fill.quantity,
                               text(toString(fill.liquidity)), text(formatFee(fill.fee)),
                               fill.counterparty, text(fill.group), text(fill.type),
                               number(fill.value), number(fill.midAfter[0]),
                               number(fill.midAfter[1]), number(fill.midAfter[2]));
        }
        out << "\n      ],\n      \"pnl\": [";
        for (std::size_t p = 0; p < seat.pnl.size(); ++p) {
            const PnlPoint& point = seat.pnl[p];
            out << (p == 0 ? "\n" : ",\n");
            out << std::format("        {{\"time_ns\": {}, \"position\": {}, \"cash\": {}, "
                               "\"fees\": {}, \"pnl\": {}, \"pnl_at_value\": {}}}",
                               point.time, point.position, point.cash,
                               text(formatFee(point.fees)), point.pnl, number(point.pnlAtValue));
        }
        out << "\n      ],\n      \"without\": {\n";
        out << std::format("        \"last_price\": {},\n        \"last_price_without\": {},\n",
                           seat.lastPrice, seat.lastPriceWithout);
        out << std::format("        \"vwap\": {},\n        \"vwap_without\": {},\n",
                           number(seat.vwap), number(seat.vwapWithout));
        out << std::format("        \"impact\": {},\n        \"prices\": [", number(seat.impact));
        for (std::size_t p = 0; p < seat.prices.size(); ++p) {
            const PricePoint& point = seat.prices[p];
            out << (p == 0 ? "\n" : ",\n");
            out << std::format("          {{\"time_ns\": {}, \"mid\": {}, \"mid_without\": {}}}",
                               point.time, number(point.mid), number(point.midWithout));
        }
        out << "\n        ]\n      }\n    }";
    }
    out << "\n  ]\n}\n";
}

} // namespace crowdbook
