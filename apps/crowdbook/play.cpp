#include "play.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"
#include "ladder.hpp"
#include "terminal.hpp"

namespace crowdbook {

namespace {

constexpr std::array kSpeeds{0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 50.0};
constexpr auto kFrame = std::chrono::milliseconds{33};

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

double nextSpeed(double speed, bool faster) {
    if (faster) {
        const auto* next = std::ranges::find_if(kSpeeds, [speed](double s) { return s > speed; });
        return next == kSpeeds.end() ? speed : *next;
    }
    double slower = speed;
    for (const double candidate : kSpeeds) {
        if (candidate < speed) {
            slower = candidate;
        }
    }
    return slower;
}

std::string describe(const OrderRejected& rejection) {
    return std::format("{} of order {} rejected: {}", toString(rejection.request),
                       rejection.clientOrderId, toString(rejection.reason));
}

// The live session: the market, what the participant has done, and what the screen shows.
class LiveSession {
public:
    LiveSession(const Scenario& scenario, SessionMarket& market, Session& record)
        : scenario_(scenario), market_(market), record_(record),
          cursor_(scenario.referencePrice),
          size_(std::min<Quantity>(5, scenario.participant.account.maxOrderQuantity)) {
        if (scenario.exchange.depthLevels == 0) {
            message_ = "no depth feed in this scenario: the ladder shows only the best prices";
        }
    }

    // Runs until the person quits.
    void run(RawTerminal& terminal, double speed) {
        Simulation& simulation = market_.run.simulation();
        Pacer pacer{wallNow(), simulation.now(), speed};
        while (true) {
            const Timestamp target = std::min(pacer.simulatedAt(wallNow()), scenario_.duration);
            if (target > simulation.now()) {
                market_.run.runUntil(target);
            }
            noteRejection();
            terminal.draw(ladder::render(screen(terminal, pacer)));
            // Wait a frame for a key, then take every other key already typed.
            for (std::optional<Key> key = terminal.readKey(kFrame); key;
                 key = terminal.readKey(std::chrono::milliseconds{0})) {
                if (!handle(*key, pacer)) {
                    return;
                }
            }
        }
    }

private:
    // play has the session's only seat.
    [[nodiscard]] const Seat& seat() const { return market_.seats.front(); }

    ladder::Screen screen(const RawTerminal& terminal, const Pacer& pacer) const {
        const Simulation& simulation = market_.run.simulation();
        const auto [rows, columns] = terminal.size();
        const auto& tape = seat().participant->tape();
        // Filled in field by field: GCC 14 at -O3 mistakes the vectors of a braced temporary
        // for uninitialized.
        ladder::Screen view;
        view.now = simulation.now();
        view.end = scenario_.duration;
        view.speed = pacer.speed();
        view.paused = pacer.paused();
        view.market = simulation.marketSeenBy(seat().agent);
        view.ledger = &simulation.ledger(seat().agent);
        view.tape.assign(tape.begin(), tape.end());
        view.referencePrice = scenario_.referencePrice;
        view.initialCash = scenario_.participant.account.initialCash;
        view.initialPosition = scenario_.participant.account.initialPosition;
        view.cursor = cursor_;
        view.size = size_;
        view.message = over() ? "the session is over: press q to see the results" : message_;
        view.rows = rows;
        view.columns = columns;
        return view;
    }

    [[nodiscard]] bool over() const {
        return market_.run.simulation().now() >= scenario_.duration;
    }

    void noteRejection() {
        const std::optional<OrderRejected>& rejection = seat().participant->lastRejection();
        if (rejection && rejection != shownRejection_) {
            shownRejection_ = rejection;
            message_ = describe(*rejection);
        }
    }

    // Sends a request now and records it. Trading stops when the session is over.
    void act(Request request) {
        if (over()) {
            return;
        }
        Simulation& simulation = market_.run.simulation();
        SessionAction action{.time = simulation.now(), .request = std::move(request)};
        const ClientOrderId id = perform(simulation, seat().agent, action);
        if (auto* order = std::get_if<NewOrder>(&action.request)) {
            order->clientOrderId = id;
        }
        record_.actions.push_back(std::move(action));
    }

    void order(Side side, OrderType type) {
        act(NewOrder{.side = side, .type = type, .price = cursor_, .quantity = size_});
        message_ = type == OrderType::Market
                       ? std::format("{} {} at the market", side == Side::Buy ? "buy" : "sell",
                                     size_)
                       : std::format("{} {} at {}", side == Side::Buy ? "bid" : "offer", size_,
                                     cursor_);
    }

    void cancel(bool everywhere) {
        std::vector<ClientOrderId> ids;
        for (const auto& [id, own] :
             market_.run.simulation().ledger(seat().agent).orders()) {
            if (own.type == OrderType::Limit && !own.cancelRequested &&
                (everywhere || own.price == cursor_)) {
                ids.push_back(id);
            }
        }
        for (const ClientOrderId id : ids) {
            act(CancelOrder{.clientOrderId = id});
        }
        message_ = std::format("cancelling {} order{}", ids.size(), ids.size() == 1 ? "" : "s");
    }

    void centerCursor() {
        const MarketSnapshot market = market_.run.simulation().marketSeenBy(seat().agent);
        if (market.bid && market.ask) {
            cursor_ = (market.bid->price + market.ask->price) / 2;
        } else if (market.lastTrade) {
            cursor_ = *market.lastTrade;
        }
    }

    // Returns false when the person quits.
    bool handle(const Key& key, Pacer& pacer) {
        switch (key.kind) {
        case Key::Kind::Up:
            ++cursor_;
            return true;
        case Key::Kind::Down:
            cursor_ = std::max<Price>(1, cursor_ - 1);
            return true;
        case Key::Kind::PageUp:
            cursor_ += 10;
            return true;
        case Key::Kind::PageDown:
            cursor_ = std::max<Price>(1, cursor_ - 10);
            return true;
        case Key::Kind::Character:
            break;
        }
        const Quantity largest = scenario_.participant.account.maxOrderQuantity;
        switch (key.character) {
        case 'q':
            return false;
        case 'b':
            order(Side::Buy, OrderType::Limit);
            break;
        case 's':
            order(Side::Sell, OrderType::Limit);
            break;
        case 'B':
            order(Side::Buy, OrderType::Market);
            break;
        case 'S':
            order(Side::Sell, OrderType::Market);
            break;
        case 'c':
            cancel(false);
            break;
        case 'C':
            cancel(true);
            break;
        case 'm':
            centerCursor();
            break;
        case '+':
        case '=':
            size_ = std::min(size_ + 1, largest);
            break;
        case '-':
        case '_':
            size_ = std::max<Quantity>(size_ - 1, 1);
            break;
        case ' ':
            pacer.setPaused(!pacer.paused(), wallNow());
            break;
        case ']':
            pacer.setSpeed(nextSpeed(pacer.speed(), true), wallNow());
            break;
        case '[':
            pacer.setSpeed(nextSpeed(pacer.speed(), false), wallNow());
            break;
        default:
            break;
        }
        return true;
    }

    const Scenario& scenario_;
    SessionMarket& market_;
    Session& record_;
    Price cursor_;
    Quantity size_;
    std::string message_;
    std::optional<OrderRejected> shownRejection_;
};

} // namespace

int play(const PlayOptions& options) {
    const std::string text = readFile(options.scenarioPath);
    Scenario scenario = parseScenario(text, options.scenarioPath);
    if (options.seed) {
        scenario.seed = *options.seed;
    }
    if (options.duration) {
        scenario.duration = *options.duration;
    }

    const OutputOptions logOnly{.logPath = options.outputs.logPath,
                                .logKinds = options.outputs.logKinds};
    Outputs outputs{logOnly, scenario};
    // Opened now, so that a path that cannot be written fails before the session rather than
    // after it.
    std::ofstream recordFile;
    if (options.recordPath) {
        recordFile.open(*options.recordPath);
        if (!recordFile) {
            throw std::runtime_error(std::format("cannot write '{}'", *options.recordPath));
        }
    }
    SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns(), outputs.sink());
    Session record{.scenario = text, .seed = scenario.seed};
    {
        RawTerminal terminal;
        LiveSession session{scenario, market, record};
        session.run(terminal, options.speed);
    }
    record.end = market.run.simulation().now();
    outputs.finish(record.end);

    std::cout << std::format("{} (seed {}): played {} of {} with {} actions\n",
                             options.scenarioPath, scenario.seed, formatDuration(record.end),
                             formatDuration(scenario.duration), record.actions.size());
    scenario.duration = record.end;
    printResults(std::cout, scenario, market.run.result());
    outputs.report(std::cout, scenario, market.run.result());
    if (options.recordPath) {
        writeSession(recordFile, record);
        if (!recordFile.flush()) {
            throw std::runtime_error(std::format("could not write all of '{}'",
                                                 *options.recordPath));
        }
        std::cout << std::format("session written to {}; crowdbook replay plays it back\n",
                                 *options.recordPath);
    }
    return 0;
}

} // namespace crowdbook
