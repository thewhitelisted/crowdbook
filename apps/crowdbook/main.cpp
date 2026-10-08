#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <format>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/parameters.hpp"
#include "crowdbook/report.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"
#include "crowdbook/version.hpp"
#include "output.hpp"
#include "connect.hpp"
#include "play.hpp"
#include "serve.hpp"

namespace crowdbook {
namespace {

constexpr std::string_view kUsage =
    "usage: crowdbook run <scenario.toml> [--seed N] [--duration D] [--log FILE]\n"
    "                     [--log-only KIND,KIND...] [--json FILE]\n"
    "                     [--prices FILE] [--depth FILE] [--sample-interval D]\n"
    "       crowdbook play <scenario.toml | session.toml --at D> [--seed N]\n"
    "                      [--duration D] [--speed X]\n"
    "                      [--record FILE] [--report FILE] [--log FILE]\n"
    "                      [--log-only KIND,KIND...]\n"
    "       crowdbook serve <scenario.toml | session.toml --at D> [--seat NAME]...\n"
    "                       [--listen HOST:PORT]\n"
    "                       [--tokens FILE] [--rate-limit N] [--seed N] [--duration D]\n"
    "                       [--speed X] [--record FILE] [--report FILE] [--log FILE]\n"
    "                       [--log-only KIND,KIND...] [--json FILE]\n"
    "       crowdbook connect <host:port> [--seat NAME] [--token TOKEN]\n"
    "       crowdbook replay <session.toml> [--log FILE] [--log-only KIND,KIND...]\n"
    "                        [--json FILE] [--prices FILE] [--depth FILE]\n"
    "                        [--sample-interval D]\n"
    "       crowdbook report <session.toml> [--json FILE] [--interval D]\n"
    "       crowdbook agents\n"
    "       crowdbook --version\n";

struct CommandLine {
    std::string path{}; // the scenario, or for replay the session
    std::optional<std::uint64_t> seed{};
    std::optional<Duration> duration{};
    std::optional<double> speed{};
    std::optional<std::string> recordPath{};
    std::vector<std::string> seats{};
    std::optional<std::string> listen{};
    std::optional<std::string> tokensPath{};
    std::optional<std::int64_t> rateLimit{};
    std::optional<std::string> token{};
    std::optional<std::string> reportPath{};
    std::optional<Duration> interval{};
    std::optional<Timestamp> at{};
    OutputOptions outputs{};
};

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> items;
    for (const auto item : std::views::split(text, ',')) {
        items.emplace_back(item.begin(), item.end());
    }
    return items;
}

// Seeds go up to the largest a scenario or session file can hold.
std::uint64_t parseSeed(std::string_view text) {
    std::int64_t seed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (error != std::errc{} || end != text.data() + text.size() || seed < 0) {
        throw UsageError(std::format("--seed needs a whole number from 0 to {}, not '{}'",
                                     std::numeric_limits<std::int64_t>::max(), text));
    }
    return static_cast<std::uint64_t>(seed);
}

double parseSpeed(std::string_view text) {
    // strtod rather than from_chars, whose floating-point form older standard libraries lack.
    const std::string copy{text};
    char* end = nullptr;
    const double speed = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || !(speed > 0.0) ||
        !std::isfinite(speed)) {
        throw UsageError(std::format("--speed needs a positive number, not '{}'", text));
    }
    return speed;
}

std::int64_t parseRateLimit(std::string_view text) {
    std::int64_t rate = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rate);
    if (error != std::errc{} || end != text.data() + text.size() || rate < 1 ||
        rate > 1'000'000) {
        throw UsageError(std::format(
            "--rate-limit needs a whole number of messages a second from 1 to 1000000, not '{}'",
            text));
    }
    return rate;
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
        } else if (arg == "--seat") {
            line.seats.emplace_back(value());
        } else if (arg == "--listen") {
            line.listen = std::string{value()};
        } else if (arg == "--report") {
            line.reportPath = std::string{value()};
        } else if (arg == "--at") {
            line.at = parseDuration(value());
        } else if (arg == "--interval") {
            line.interval = parseDuration(value());
        } else if (arg == "--token") {
            line.token = std::string{value()};
        } else if (arg == "--tokens") {
            line.tokensPath = std::string{value()};
        } else if (arg == "--rate-limit") {
            line.rateLimit = parseRateLimit(value());
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
        throw UsageError(
            std::format("{} needs {}", command, command == "connect" ? "host:port" : "a file"));
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

// A rewound session keeps its seed, its seats and the actions it rewinds through.
void checkRewind(const CommandLine& line) {
    if (line.at && line.seed) {
        throw UsageError(
            "--at rewinds a session, which keeps its seed: --seed cannot change it");
    }
    if (line.at && line.duration) {
        throw UsageError("--at rewinds a session, which keeps its duration and the day's "
                         "schedule with it: --duration cannot change it");
    }
    if (line.at && !line.seats.empty()) {
        throw UsageError(
            "--at rewinds a session, which keeps its seats: --seat cannot change them");
    }
}

int playCommand(std::span<char*> args) {
    const CommandLine line = parseCommandLine(
        args, "play",
        {"--at", "--seed", "--duration", "--speed", "--record", "--report", "--log", "--log-only"});
    checkRewind(line);
    return play({.scenarioPath = line.path,
                 .seed = line.seed,
                 .duration = line.duration,
                 .rewindAt = line.at,
                 .speed = line.speed.value_or(1.0),
                 .recordPath = line.recordPath,
                 .reportPath = line.reportPath,
                 .outputs = line.outputs});
}

int serveCommand(std::span<char*> args) {
    const CommandLine line = parseCommandLine(
        args, "serve",
        {"--at", "--seat", "--listen", "--tokens", "--rate-limit", "--seed", "--duration",
         "--speed", "--record", "--report", "--log", "--log-only", "--json"});
    checkRewind(line);
    ServeOptions options{.scenarioPath = line.path,
                         .seed = line.seed,
                         .duration = line.duration,
                         .rewindAt = line.at,
                         .speed = line.speed.value_or(1.0),
                         .seats = line.seats,
                         .tokensPath = line.tokensPath,
                         .rateLimit = line.rateLimit,
                         .recordPath = line.recordPath,
                         .reportPath = line.reportPath,
                         .outputs = line.outputs};
    if (line.listen) {
        parseListen(*line.listen, options.host, options.port);
    }
    return serve(options);
}

int reportCommand(std::span<char*> args) {
    const CommandLine line = parseCommandLine(args, "report", {"--json", "--interval"});
    const Session session = loadSession(line.path);
    std::ofstream json;
    if (line.outputs.jsonPath) {
        json.open(*line.outputs.jsonPath);
        if (!json) {
            throw std::runtime_error(std::format("cannot write '{}'", *line.outputs.jsonPath));
        }
    }
    const SessionReport report =
        makeReport(session, AgentRegistry::withBuiltIns(), line.interval.value_or(kSecond));
    std::cout << std::format("{} (seed {}): {} with {} actions\n", line.path, session.seed,
                             formatDuration(session.end), session.actions.size());
    printReport(std::cout, report);
    if (line.outputs.jsonPath) {
        writeReportJson(json, report);
        if (!json.flush()) {
            throw std::runtime_error(
                std::format("could not write all of '{}'", *line.outputs.jsonPath));
        }
        std::cout << std::format("report written to {}\n", *line.outputs.jsonPath);
    }
    return 0;
}

int connectCommand(std::span<char*> args) {
    const CommandLine line = parseCommandLine(args, "connect", {"--seat", "--token"});
    if (line.seats.size() > 1) {
        throw UsageError("connect takes one --seat");
    }
    ConnectOptions options;
    parseListen(line.path, options.host, options.port);
    if (!line.seats.empty()) {
        options.seat = line.seats.front();
    }
    options.token = line.token.value_or("");
    return connect(options);
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
    if (command == "serve") {
        return serveCommand(args.subspan(1));
    }
    if (command == "connect") {
        return connectCommand(args.subspan(1));
    }
    if (command == "report") {
        return reportCommand(args.subspan(1));
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
