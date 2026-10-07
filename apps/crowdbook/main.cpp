#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
};

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
    return options;
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

    std::ofstream logFile;
    std::optional<CsvEventLog> log;
    if (options.logPath) {
        logFile.open(*options.logPath);
        if (!logFile) {
            throw std::runtime_error(std::format("cannot write '{}'", *options.logPath));
        }
        log.emplace(logFile);
    }

    const auto started = std::chrono::steady_clock::now();
    const RunResult result =
        runScenario(scenario, AgentRegistry::withBuiltIns(), log ? &*log : nullptr);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;

    std::cout << std::format("{} (seed {}): simulated {} in {:.2f}s\n", options.scenarioPath,
                             scenario.seed, formatDuration(scenario.duration), elapsed.count());
    std::cout << std::format("{} trades, {} lots traded, last price {}", result.trades,
                             result.volume, result.lastPrice);
    if (result.finalValue) {
        std::cout << std::format(", true value {:.1f}", *result.finalValue);
    }
    std::cout << "\n\n";
    std::cout << std::format("{:<20} {:>7} {:>10} {:>14} {:>12}\n", "group", "agents",
                             "position", "cash", "pnl");
    for (const GroupResult& group : result.groups) {
        std::cout << std::format("{:<20} {:>7} {:>10} {:>14} {:>+12}\n", group.name,
                                 group.agents.size(), group.position, group.cash, group.pnl);
    }
    std::cout << "\ncash and pnl are in tick-lots; pnl values positions at the last price\n";
    if (options.logPath) {
        std::cout << std::format("event log written to {}\n", *options.logPath);
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
