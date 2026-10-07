#include "output.hpp"

#include "crowdbook/agent_registry.hpp"

#include <format>
#include <iostream>

namespace crowdbook {

namespace {

std::ofstream openForWriting(const std::string& path) {
    std::ofstream file{path};
    if (!file) {
        throw std::runtime_error(std::format("cannot write '{}'", path));
    }
    return file;
}

// Throws if anything written to the file at `path`, if there is one, failed to reach it, as on a
// full disk.
void checkWritten(std::ofstream& file, const std::optional<std::string>& path) {
    if (path && !file.flush()) {
        throw std::runtime_error(std::format("could not write all of '{}'", *path));
    }
}

} // namespace

Outputs::Outputs(const OutputOptions& options, const Scenario& scenario) : options_(options) {
    if (options.logPath) {
        logFile_ = openForWriting(*options.logPath);
        sinks_.add(log_.emplace(logFile_, options.logKinds));
    }
    if (options.pricesPath) {
        pricesFile_ = openForWriting(*options.pricesPath);
        sinks_.add(prices_.emplace(pricesFile_, options.sampleInterval));
    }
    if (options.depthPath) {
        if (scenario.exchange.depthLevels == 0) {
            throw std::runtime_error("--depth needs a depth feed: add depth_levels to the "
                                     "scenario's [exchange] section");
        }
        depthFile_ = openForWriting(*options.depthPath);
        sinks_.add(
            depth_.emplace(depthFile_, options.sampleInterval, scenario.exchange.depthLevels));
    }
    if (options.jsonPath) {
        jsonFile_ = openForWriting(*options.jsonPath);
    }
}

void Outputs::finish(Timestamp end) {
    if (prices_) {
        prices_->finish(end);
    }
    if (depth_) {
        depth_->finish(end);
    }
    checkWritten(logFile_, options_.logPath);
    checkWritten(pricesFile_, options_.pricesPath);
    checkWritten(depthFile_, options_.depthPath);
}

void Outputs::report(std::ostream& out, const Scenario& scenario, const RunResult& result) {
    if (options_.logPath) {
        out << std::format("event log written to {}\n", *options_.logPath);
    }
    if (options_.pricesPath) {
        out << std::format("prices written to {}\n", *options_.pricesPath);
    }
    if (options_.depthPath) {
        out << std::format("depth written to {}\n", *options_.depthPath);
    }
    if (options_.jsonPath) {
        writeResultJson(jsonFile_, scenario, result);
        checkWritten(jsonFile_, options_.jsonPath);
        out << std::format("results written to {}\n", *options_.jsonPath);
    }
}

void printResults(std::ostream& out, const Scenario& scenario, const RunResult& result) {
    out << std::format("{} trades, {} lots traded, last price {}", result.trades, result.volume,
                       result.lastPrice);
    if (result.finalValue) {
        out << std::format(", true value {:.1f}", *result.finalValue);
    }
    out << "\n\n";
    // Fee columns appear only when the exchange charges fees.
    const bool fees = scenario.exchange.makerFee != 0 || scenario.exchange.takerFee != 0;
    const auto signedFee = [](Fee fee) { return (fee > 0 ? "+" : "") + formatFee(fee); };
    out << std::format("{:<20} {:>7} {:>10} {:>14} {:>12}", "group", "agents", "position", "cash",
                       "pnl");
    out << (fees ? std::format(" {:>12} {:>12}\n", "fees", "net pnl") : "\n");
    for (const GroupResult& group : result.groups) {
        out << std::format("{:<20} {:>7} {:>10} {:>14} {:>+12}", group.name, group.agents.size(),
                           group.position, group.cash, group.pnl);
        out << (fees ? std::format(" {:>12} {:>12}\n", formatFee(group.fees),
                                   signedFee(group.pnl * kFeeUnitsPerTickLot - group.fees))
                     : "\n");
    }
    out << "\ncash and pnl are in tick-lots; pnl values positions at the last price"
        << (fees ? ", and net pnl is pnl minus fees\n" : "\n");
    if (result.scores.empty()) {
        return;
    }
    out << std::format("\n{:<20} {:>12} {:>12} {:>10} {:>10} {:>12} {:>11}\n", "seat", "score",
                       "pnl", "inventory", "close", "paper pnl", "unfinished");
    for (const SeatScore& seat : result.scores) {
        const Score& score = seat.score;
        out << std::format("{:<20} {:>12} {:>12} {:>10} {:>10} {:>12} {:>11}", seat.seat,
                           formatPoints(score.total), formatPoints(score.pnl),
                           formatFee(score.inventory), formatFee(score.close),
                           formatPoints(score.paper), formatFee(score.unfinished));
        out << (score.stoppedAt
                    ? std::format("  stopped at {}\n", formatDuration(*score.stoppedAt))
                    : "\n");
    }
    out << "scores are in tick-lots: pnl net of fees at the mark, less the penalties and, with a "
           "target, the paper pnl\n";
}

void printBriefing(std::ostream& out, const Scenario& scenario) {
    if (scenario.challenge) {
        out << std::format("challenge: {}\n\n{}\n", scenario.challenge->name,
                           scenario.challenge->briefing);
        if (!scenario.challenge->briefing.ends_with('\n')) {
            out << '\n';
        }
    }
    if (!scenario.scoring) {
        return;
    }
    const ScoringConfig& scoring = *scenario.scoring;
    out << std::format("scored on pnl net of fees, with positions valued at {}",
                       scoring.mark == Mark::Value ? "the true value at the end"
                                                   : "the last trade price");
    if (scoring.inventoryPenalty != 0) {
        out << std::format("; holding costs {} a lot a second",
                           formatFee(scoring.inventoryPenalty));
    }
    if (scoring.closePenalty != 0) {
        out << std::format("; each lot held at the end costs {}", formatFee(scoring.closePenalty));
    }
    if (scoring.maxLoss != 0) {
        out << std::format("; a loss of {} stops you", scoring.maxLoss);
    }
    if (const auto& target = scoring.target) {
        out << std::format("; the target is to {} {} lots, measured against {}",
                           toString(target->side), target->quantity,
                           target->benchmark == Benchmark::Vwap ? "the market's VWAP"
                                                                : "the reference price");
        if (target->unfinishedPenalty != 0) {
            out << std::format(", and each lot not done costs {}",
                               formatFee(target->unfinishedPenalty));
        }
    }
    out << " (all in tick-lots)\n\n";
}

void printReport(std::ostream& out, const SessionReport& report) {
    const auto ticks = [](const std::optional<double>& value) {
        return value ? std::format("{:+.2f}", *value) : std::string{"-"};
    };
    for (const SeatReport& seat : report.seats) {
        Quantity lots = 0;
        for (const ReportFill& fill : seat.fills) {
            lots += fill.quantity;
        }
        out << std::format("\nseat {}: {} fills, {} lots", seat.seat, seat.fills.size(), lots);
        if (seat.score) {
            out << std::format(", score {}", formatPoints(seat.score->total));
        }
        out << "\n";
        if (!seat.counterparties.empty()) {
            out << std::format("  {:<16} {:<18} {:>6} {:>7} {:>7} {:>12} {:>14}\n", "traded with",
                               "type", "fills", "bought", "sold", "their edge", "your markout");
            for (const Counterparty& party : seat.counterparties) {
                out << std::format("  {:<16} {:<18} {:>6} {:>7} {:>7} {:>12} {:>14}\n",
                                   party.group, party.type, party.fills, party.bought, party.sold,
                                   ticks(party.edge), ticks(party.markout));
            }
            out << "  their edge: how far the price was from the true value in their favour\n"
                   "  your markout: the mid ten seconds later against your price\n"
                   "  both in ticks a lot\n";
        }
        if (!seat.pnl.empty()) {
            const PnlPoint& last = seat.pnl.back();
            out << std::format("  pnl at the end {:+.1f}", last.pnl);
            if (last.pnlAtValue) {
                out << std::format(", {:+.1f} at the true value", *last.pnlAtValue);
            }
            out << " (tick-lots, net of fees)\n";
        }
        out << std::format("  without you: last price {} instead of {}", seat.lastPriceWithout,
                           seat.lastPrice);
        if (seat.impact) {
            out << std::format("; you moved the mid {:+.2f} ticks on average", *seat.impact);
        }
        out << "\n";
    }
}

void writeReport(std::ofstream& file, const std::string& path, const Session& session) {
    const SessionReport report = makeReport(session, AgentRegistry::withBuiltIns());
    printReport(std::cout, report);
    writeReportJson(file, report);
    if (!file.flush()) {
        throw std::runtime_error(std::format("could not write all of '{}'", path));
    }
    std::cout << std::format("report written to {}\n", path);
}

std::string formatPoints(Points points) {
    return (points > 0 ? "+" : "") + formatFee(points);
}

std::string formatDuration(Duration duration) {
    if (duration % kSecond == 0) {
        return std::format("{}s", duration / kSecond);
    }
    if (duration % kMillisecond == 0) {
        return std::format("{}ms", duration / kMillisecond);
    }
    if (duration >= kSecond) {
        return std::format("{:.3f}s", static_cast<double>(duration) / kSecond);
    }
    if (duration % kMicrosecond == 0) {
        return std::format("{}us", duration / kMicrosecond);
    }
    return std::format("{}ns", duration);
}

} // namespace crowdbook
