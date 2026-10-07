#include "serve.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/server.hpp"
#include "crowdbook/session.hpp"

namespace crowdbook {

namespace {

std::atomic<bool> interrupted{false};

extern "C" void onInterrupt(int /*signal*/) {
    interrupted = true;
}

std::string readFile(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error(std::format("cannot read '{}'", path));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Each line names a seat and its token, separated by whitespace; blank lines and lines starting
// with '#' are skipped.
std::map<std::string, std::string> readTokens(const std::string& path) {
    std::istringstream lines{readFile(path)};
    std::map<std::string, std::string> tokens;
    std::string line;
    for (int number = 1; std::getline(lines, line); ++number) {
        std::istringstream words{line};
        std::string seat;
        std::string token;
        std::string extra;
        if (!(words >> seat) || seat.starts_with('#')) {
            continue;
        }
        if (!(words >> token) || (words >> extra) || token.size() > protocol::kMaxTokenLength) {
            throw std::runtime_error(
                std::format("{}:{}: each line needs a seat and a token, and nothing else", path,
                            number));
        }
        if (!tokens.emplace(seat, token).second) {
            throw std::runtime_error(
                std::format("{}:{}: the seat '{}' has two tokens", path, number, seat));
        }
    }
    return tokens;
}

std::string listSeats(const std::vector<std::string>& seats) {
    std::string list;
    for (const std::string& seat : seats) {
        list += (list.empty() ? "" : ", ") + seat;
    }
    return list;
}

} // namespace

void parseListen(std::string_view text, std::string& host, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        throw UsageError(std::format("--listen needs host:port, not '{}'", text));
    }
    std::string_view name = text.substr(0, colon);
    const std::string_view number = text.substr(colon + 1);
    if (name.starts_with('[') && name.ends_with(']')) {
        name = name.substr(1, name.size() - 2);
    }
    unsigned value = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (error != std::errc{} || end != number.data() + number.size() || value > 65'535) {
        throw UsageError(std::format("--listen needs a port from 0 to 65535, not '{}'", number));
    }
    host = name.empty() ? "0.0.0.0" : std::string{name};
    port = static_cast<std::uint16_t>(value);
}

int serve(const ServeOptions& options) {
    // A scenario to start, or with rewindAt a session to rewind.
    std::optional<Session> from;
    if (options.rewindAt) {
        from = loadSession(options.scenarioPath);
    }
    const std::string text = from ? from->scenario : readFile(options.scenarioPath);
    Scenario scenario = parseScenario(text, options.scenarioPath);
    if (from) {
        scenario.seed = from->seed;
    }
    if (options.seed) {
        scenario.seed = *options.seed;
    }
    if (options.duration) {
        scenario.duration = *options.duration;
    }
    GatewayOptions gatewayOptions;
    if (!options.seats.empty()) {
        gatewayOptions.seats = options.seats;
    }
    if (options.tokensPath) {
        gatewayOptions.tokens = readTokens(*options.tokensPath);
    }
    gatewayOptions.speed = options.speed;
    if (from) {
        gatewayOptions.rewind = from;
        gatewayOptions.rewindAt = *options.rewindAt;
    }
    if (options.rateLimit) {
        gatewayOptions.maxMessagesPerSecond = *options.rateLimit;
    }

    const OutputOptions outputOptions{.logPath = options.outputs.logPath,
                                      .logKinds = options.outputs.logKinds,
                                      .jsonPath = options.outputs.jsonPath};
    Outputs outputs{outputOptions, scenario};
    std::ofstream recordFile;
    if (options.recordPath) {
        recordFile.open(*options.recordPath);
        if (!recordFile) {
            throw std::runtime_error(std::format("cannot write '{}'", *options.recordPath));
        }
    }
    std::ofstream reportFile;
    if (options.reportPath) {
        reportFile.open(*options.reportPath);
        if (!reportFile) {
            throw std::runtime_error(std::format("cannot write '{}'", *options.reportPath));
        }
    }
    Gateway gateway{scenario, text, AgentRegistry::withBuiltIns(), gatewayOptions,
                    outputs.sink()};

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
    crowdbook::serve(gateway, {.host = options.host, .port = options.port}, interrupted,
                     [&](std::uint16_t port) {
                         std::cout << std::format(
                             "{} (seed {}): serving on {}:{} to seats {}; the clock starts when "
                             "every seat is claimed, and ctrl-c ends the session\n",
                             options.scenarioPath, scenario.seed, options.host, port,
                             listSeats(from ? from->seats : gatewayOptions.seats))
                                   << std::flush;
                     });
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    const Session& record = gateway.session();
    outputs.finish(record.end);
    std::cout << std::format("{} (seed {}): served {} of {} with {} actions\n",
                             options.scenarioPath, scenario.seed, formatDuration(record.end),
                             formatDuration(scenario.duration), record.actions.size());
    scenario.duration = record.end;
    const RunResult result = gateway.result();
    printResults(std::cout, scenario, result);
    outputs.report(std::cout, scenario, result);
    if (options.recordPath) {
        writeSession(recordFile, record);
        if (!recordFile.flush()) {
            throw std::runtime_error(std::format("could not write all of '{}'",
                                                 *options.recordPath));
        }
        std::cout << std::format("session written to {}; crowdbook replay plays it back\n",
                                 *options.recordPath);
    }
    if (options.reportPath) {
        writeReport(reportFile, *options.reportPath, record);
    }
    return 0;
}

} // namespace crowdbook
