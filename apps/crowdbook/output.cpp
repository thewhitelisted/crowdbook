#include "output.hpp"

#include <format>

namespace crowdbook {

namespace {

std::ofstream openForWriting(const std::string& path) {
    std::ofstream file{path};
    if (!file) {
        throw std::runtime_error(std::format("cannot write '{}'", path));
    }
    return file;
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
}

void Outputs::finish(Timestamp end) {
    if (prices_) {
        prices_->finish(end);
    }
    if (depth_) {
        depth_->finish(end);
    }
}

void Outputs::report(std::ostream& out, const Scenario& scenario, const RunResult& result) const {
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
        std::ofstream json = openForWriting(*options_.jsonPath);
        writeResultJson(json, scenario, result);
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
