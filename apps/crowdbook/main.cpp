#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/parameters.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"
#include "crowdbook/version.hpp"
#include "output.hpp"
#include "play.hpp"

namespace crowdbook {
namespace {

constexpr std::string_view kUsage =
    "usage: crowdbook run <scenario.toml> [--seed N] [--duration D] [--log FILE]\n"
    "                     [--log-only KIND,KIND...] [--json FILE]\n"
    "                     [--prices FILE] [--depth FILE] [--sample-interval D]\n"
    "       crowdbook play <scenario.toml> [--seed N] [--duration D] [--speed X]\n"
    "                      [--record FILE] [--log FILE] [--log-only KIND,KIND...]\n"
    "       crowdbook replay <session.toml> [--log FILE] [--log-only KIND,KIND...]\n"
    "                        [--json FILE] [--prices FILE] [--depth FILE]\n"
    "                        [--sample-interval D]\n"
    "       crowdbook agents\n"
    "       crowdbook --version\n";

struct CommandLine {
    std::string path{}; // the scenario, or for replay the session
    std::optional<std::uint64_t> seed{};
    std::optional<Duration> duration{};
    std::optional<double> speed{};
    std::optional<std::string> recordPath{};
    OutputOptions outputs{};
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

double parseSpeed(std::string_view text) {
    // strtod rather than from_chars, whose floating-point form older standard libraries lack.
    const std::string copy{text};
    char* end = nullptr;
    const double speed = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || !(speed > 0.0)) {
        throw UsageError(std::format("--speed needs a positive number, not '{}'", text));
    }
    return speed;
}

// Reads one command's arguments. `allowed` lists the options it takes.
CommandLine parseCommandLine(std::span<char*> args, std::string_view command,
                             std::initializer_list<std::string_view> allowed) {
    CommandLine line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg.starts_with("--") && std::ranges::find(allowed, arg) == allowed.end()) {
            throw UsageError(std::format("{} does not take {}", command, arg));
        }
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw UsageError(std::format("{} needs a value", arg));
            }
            return args[++i];
        };
        if (arg == "--seed") {
            line.seed = parseSeed(value());
        } else if (arg == "--duration") {
            line.duration = parseDuration(value());
        } else if (arg == "--speed") {
            line.speed = parseSpeed(value());
        } else if (arg == "--record") {
            line.recordPath = std::string{value()};
        } else if (arg == "--log") {
            line.outputs.logPath = std::string{value()};
        } else if (arg == "--log-only") {
            line.outputs.logKinds = splitList(value());
        } else if (arg == "--json") {
            line.outputs.jsonPath = std::string{value()};
        } else if (arg == "--prices") {
            line.outputs.pricesPath = std::string{value()};
        } else if (arg == "--depth") {
            line.outputs.depthPath = std::string{value()};
        } else if (arg == "--sample-interval") {
            line.outputs.sampleInterval = parseDuration(value());
        } else if (line.path.empty()) {
            line.path = arg;
        } else {
            throw UsageError(std::format("unexpected argument '{}'", arg));
        }
    }
    if (line.path.empty()) {
        throw UsageError(std::format("{} needs a file", command));
    }
    if (!line.outputs.logKinds.empty() && !line.outputs.logPath) {
        throw UsageError("--log-only needs --log");
    }
    return line;
}

int run(std::span<char*> args) {
    const CommandLine line =
        parseCommandLine(args, "run",
                         {"--seed", "--duration", "--log", "--log-only", "--json", "--prices",
                          "--depth", "--sample-interval"});
    Scenario scenario = loadScenario(line.path);
    if (line.seed) {
        scenario.seed = *line.seed;
    }
    if (line.duration) {
        scenario.duration = *line.duration;
    }

    Outputs outputs{line.outputs, scenario};
    const auto started = std::chrono::steady_clock::now();
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), outputs.sink());
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    outputs.finish(scenario.duration);

    std::cout << std::format("{} (seed {}): simulated {} in {:.2f}s\n", line.path, scenario.seed,
                             formatDuration(scenario.duration), elapsed.count());
    printResults(std::cout, scenario, result);
    outputs.report(std::cout, scenario, result);
    return 0;
}

int replay(std::span<char*> args) {
    const CommandLine line = parseCommandLine(
        args, "replay",
        {"--log", "--log-only", "--json", "--prices", "--depth", "--sample-interval"});
    const Session session = loadSession(line.path);
    Scenario scenario = parseScenario(session.scenario, line.path);
    scenario.seed = session.seed;
    scenario.duration = session.end;

    Outputs outputs{line.outputs, scenario};
    const RunResult result = replaySession(session, AgentRegistry::withBuiltIns(), outputs.sink());
    outputs.finish(session.end);

    std::cout << std::format("{} (seed {}): replayed {} with {} actions\n", line.path,
                             session.seed, formatDuration(session.end), session.actions.size());
    printResults(std::cout, scenario, result);
    outputs.report(std::cout, scenario, result);
    return 0;
}

int playCommand(std::span<char*> args) {
    const CommandLine line = parseCommandLine(
        args, "play", {"--seed", "--duration", "--speed", "--record", "--log", "--log-only"});
    return play({.scenarioPath = line.path,
                 .seed = line.seed,
                 .duration = line.duration,
                 .speed = line.speed.value_or(1.0),
                 .recordPath = line.recordPath,
                 .outputs = line.outputs});
}

int runCommand(std::span<char*> args) {
    if (args.empty()) {
        throw UsageError("no command given");
    }
    const std::string_view command = args[0];
    if (command == "run") {
        return run(args.subspan(1));
    }
    if (command == "replay") {
        return replay(args.subspan(1));
    }
    if (command == "play") {
        return playCommand(args.subspan(1));
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
