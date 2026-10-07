#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/parameters.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/version.hpp"

namespace crowdbook {
namespace {

constexpr std::string_view kUsage =
    "usage: crowdbook run <scenario.toml> [--seed N] [--duration D] [--log FILE]\n"
    "                     [--log-only KIND,KIND...] [--json FILE]\n"
    "                     [--prices FILE] [--price-interval D]\n"
    "       crowdbook agents\n"
    "       crowdbook --version\n";

// A mistake in the command line; main prints the message followed by the usage.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RunOptions {
    std::string scenarioPath{};
    std::optional<std::uint64_t> seed{};
    std::optional<Duration> duration{};
    std::optional<std::string> logPath{};
    std::vector<std::string> logKinds{}; // empty logs every kind
    std::optional<std::string> jsonPath{};
    std::optional<std::string> pricesPath{};
    Duration priceInterval = kSecond;
};

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> items;
    for (const auto item : std::views::split(text, ',')) {
        items.emplace_back(item.begin(), item.end());
    }
    return items;
}

std::uint64_t parseSeed(std::string_view text) {
    std::uint64_t seed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw UsageError(std::format("--seed needs a whole number, not '{}'", text));
    }
    return seed;
}

RunOptions parseRunOptions(std::span<char*> args) {
    RunOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw UsageError(std::format("{} needs a value", arg));
            }
            return args[++i];
        };
        if (arg == "--seed") {
            options.seed = parseSeed(value());
        } else if (arg == "--duration") {
            options.duration = parseDuration(value());
        } else if (arg == "--log") {
            options.logPath = std::string{value()};
        } else if (arg == "--log-only") {
            options.logKinds = splitList(value());
        } else if (arg == "--json") {
            options.jsonPath = std::string{value()};
        } else if (arg == "--prices") {
            options.pricesPath = std::string{value()};
        } else if (arg == "--price-interval") {
            options.priceInterval = parseDuration(value());
        } else if (arg.starts_with("--")) {
            throw UsageError(std::format("unknown option {}", arg));
        } else if (options.scenarioPath.empty()) {
            options.scenarioPath = arg;
        } else {
            throw UsageError(std::format("unexpected argument '{}'", arg));
        }
    }
    if (options.scenarioPath.empty()) {
        throw UsageError("run needs a scenario file");
    }
    if (!options.logKinds.empty() && !options.logPath) {
        throw UsageError("--log-only needs --log");
    }
    return options;
}

std::ofstream openForWriting(const std::string& path) {
    std::ofstream file{path};
    if (!file) {
        throw std::runtime_error(std::format("cannot write '{}'", path));
    }
    return file;
}

std::string formatDuration(Duration duration) {
    if (duration % kSecond == 0) {
        return std::format("{}s", duration / kSecond);
    }
    if (duration % kMillisecond == 0) {
        return std::format("{}ms", duration / kMillisecond);
    }
    if (duration % kMicrosecond == 0) {
        return std::format("{}us", duration / kMicrosecond);
    }
    return std::format("{}ns", duration);
}

int run(std::span<char*> args) {
    const RunOptions options = parseRunOptions(args);
    Scenario scenario = loadScenario(options.scenarioPath);
    if (options.seed) {
        scenario.seed = *options.seed;
    }
    if (options.duration) {
        scenario.duration = *options.duration;
    }

    BroadcastSink sinks;
    std::ofstream logFile;
    std::optional<CsvEventLog> log;
    if (options.logPath) {
        logFile = openForWriting(*options.logPath);
        sinks.add(log.emplace(logFile, options.logKinds));
    }
    std::ofstream pricesFile;
    std::optional<PriceSampler> prices;
    if (options.pricesPath) {
        pricesFile = openForWriting(*options.pricesPath);
        sinks.add(prices.emplace(pricesFile, options.priceInterval));
    }

    const auto started = std::chrono::steady_clock::now();
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), &sinks);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    if (prices) {
        prices->finish(scenario.duration);
    }

    std::cout << std::format("{} (seed {}): simulated {} in {:.2f}s\n", options.scenarioPath,
                             scenario.seed, formatDuration(scenario.duration), elapsed.count());
    std::cout << std::format("{} trades, {} lots traded, last price {}", result.trades,
                             result.volume, result.lastPrice);
    if (result.finalValue) {
        std::cout << std::format(", true value {:.1f}", *result.finalValue);
    }
    std::cout << "\n\n";
    // Fee columns appear only when the exchange charges fees.
    const bool fees = scenario.exchange.makerFee != 0 || scenario.exchange.takerFee != 0;
    const auto signedFee = [](Fee fee) { return (fee > 0 ? "+" : "") + formatFee(fee); };
    std::cout << std::format("{:<20} {:>7} {:>10} {:>14} {:>12}", "group", "agents", "position",
                             "cash", "pnl");
    std::cout << (fees ? std::format(" {:>12} {:>12}\n", "fees", "net pnl") : "\n");
    for (const GroupResult& group : result.groups) {
        std::cout << std::format("{:<20} {:>7} {:>10} {:>14} {:>+12}", group.name,
                                 group.agents.size(), group.position, group.cash, group.pnl);
        std::cout << (fees ? std::format(" {:>12} {:>12}\n", formatFee(group.fees),
                                         signedFee(group.pnl * kFeeUnitsPerTickLot - group.fees))
                           : "\n");
    }
    std::cout << "\ncash and pnl are in tick-lots; pnl values positions at the last price"
              << (fees ? ", and net pnl is pnl minus fees\n" : "\n");
    if (options.logPath) {
        std::cout << std::format("event log written to {}\n", *options.logPath);
    }
    if (options.pricesPath) {
        std::cout << std::format("prices written to {}\n", *options.pricesPath);
    }
    if (options.jsonPath) {
        std::ofstream json = openForWriting(*options.jsonPath);
        writeResultJson(json, scenario, result);
        std::cout << std::format("results written to {}\n", *options.jsonPath);
    }
    return 0;
}

int runCommand(std::span<char*> args) {
    if (args.empty()) {
        throw UsageError("no command given");
    }
    const std::string_view command = args[0];
    if (command == "run") {
        return run(args.subspan(1));
    }
    if (command == "agents") {
        std::cout << "agent types:\n";
        for (const std::string& type : AgentRegistry::withBuiltIns().types()) {
            std::cout << "  " << type << '\n';
        }
        std::cout << "docs/scenarios.md lists their parameters\n";
        return 0;
    }
    if (command == "--version") {
        std::cout << "crowdbook " << version() << '\n';
        return 0;
    }
    if (command == "--help" || command == "-h") {
        std::cout << kUsage;
        return 0;
    }
    throw UsageError(std::format("unknown command '{}'", command));
}

} // namespace
} // namespace crowdbook

int main(int argc, char* argv[]) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    try {
        return crowdbook::runCommand(args.subspan(1));
    } catch (const crowdbook::UsageError& error) {
        std::cerr << "crowdbook: " << error.what() << '\n' << crowdbook::kUsage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "crowdbook: " << error.what() << '\n';
        return 1;
    }
}
