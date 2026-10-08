#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/report.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

// One benchmark for each feature a run can use on top of the market itself, so that a change
// that slows one down shows up here: the event log, the protocol's encoding and decoding, a
// trading day with its auctions, session reports and a large crowd.

namespace crowdbook {
namespace {

std::string example(const std::string& path) { return std::string{CROWDBOOK_SOURCE_DIR} + path; }

std::string readText(const std::string& path) {
    std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Counts what is written to it and keeps none of it.
class CountingBuffer final : public std::streambuf {
public:
    [[nodiscard]] std::int64_t bytes() const noexcept { return bytes_; }

protected:
    std::streamsize xsputn(const char* /*text*/, std::streamsize count) override {
        bytes_ += count;
        return count;
    }
    int_type overflow(int_type character) override {
        ++bytes_;
        return character;
    }

private:
    std::int64_t bytes_ = 0;
};

// The market making challenge for a minute, with every row of the event log written.
void BM_LoggedChallenge(benchmark::State& state) {
    Scenario scenario = loadScenario(example("/examples/challenges/market_making.toml"));
    scenario.duration = 60 * kSecond;
    std::int64_t bytes = 0;
    for (auto _ : state) {
        CountingBuffer buffer;
        std::ostream out{&buffer};
        CsvEventLog log{out};
        benchmark::DoNotOptimize(runScenario(scenario, AgentRegistry::withBuiltIns(), &log));
        bytes += buffer.bytes();
    }
    state.SetBytesProcessed(bytes);
}
BENCHMARK(BM_LoggedChallenge)->Unit(benchmark::kMillisecond);

// A depth update ten levels a side, the largest message a server sends often.
void BM_EncodeDepth(benchmark::State& state) {
    BookDepth depth;
    std::vector<LevelSummary> bids;
    std::vector<LevelSummary> asks;
    for (Price level = 0; level < 10; ++level) {
        bids.push_back({.price = 9'999 - level, .quantity = 17 + level, .orderCount = 3});
        asks.push_back({.price = 10'001 + level, .quantity = 23 + level, .orderCount = 4});
    }
    depth.bids = bids;
    depth.asks = asks;
    const protocol::ServerMessage message =
        protocol::MarketMessage{.time = 123'456'789'012, .event = depth};
    std::string out;
    std::int64_t bytes = 0;
    for (auto _ : state) {
        out.clear();
        protocol::encodeTo(out, message);
        bytes += static_cast<std::int64_t>(out.size());
        benchmark::DoNotOptimize(out.data());
    }
    state.SetBytesProcessed(bytes);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EncodeDepth);

// A client's new order, as the server reads every one.
void BM_DecodeNewOrder(benchmark::State& state) {
    const std::string line = protocol::encode(protocol::ClientMessage{
        NewOrder{.clientOrderId = 17, .side = Side::Buy, .price = 10'003, .quantity = 5}});
    for (auto _ : state) {
        benchmark::DoNotOptimize(protocol::decodeClient(line));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeNewOrder);

// One order through the gateway and back with no latency in the market: the line in, the
// acknowledgement out, then the same for its cancel. What the server adds to a round trip, short
// of the sockets.
void BM_GatewayRoundTrip(benchmark::State& state) {
    Scenario scenario = loadScenario(example("/examples/scenarios/playable.toml"));
    scenario.participant.latency = {};
    Gateway gateway{scenario, "", AgentRegistry::withBuiltIns(), {}};
    const ConnectionId client = gateway.connect(0);
    gateway.receive(client, protocol::encode(protocol::Hello{.seat = "you"}), 0);
    std::int64_t wall = 0;
    ClientOrderId id = 1;
    std::vector<ConnectionId> ready;
    const auto roundTrip = [&](const protocol::ClientMessage& message) {
        wall += kMicrosecond;
        gateway.receive(client, protocol::encode(message), wall);
        gateway.advance(wall);
        ready.clear();
        gateway.takeReady(ready);
        benchmark::DoNotOptimize(gateway.pendingOutput(client).size());
        gateway.consumeOutput(client, gateway.pendingOutput(client).size());
    };
    for (auto _ : state) {
        roundTrip(NewOrder{.clientOrderId = id, .side = Side::Buy, .price = 5'000, .quantity = 1});
        roundTrip(CancelOrder{.clientOrderId = id});
        ++id;
    }
    state.SetItemsProcessed(2 * state.iterations());
}
BENCHMARK(BM_GatewayRoundTrip);

// The memory a process holds, resident, in bytes.
std::int64_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
              &count);
    return static_cast<std::int64_t>(info.resident_size);
#else
    std::ifstream statm{"/proc/self/statm"};
    std::int64_t size = 0;
    std::int64_t resident = 0;
    statm >> size >> resident;
    return resident * ::sysconf(_SC_PAGESIZE);
#endif
}

// How many markets fit in memory: a hundred of the market making challenge, each served to one
// client, run side by side for a simulated minute. The counter is the memory each adds.
void BM_MemoryPerMarket(benchmark::State& state) {
    const Scenario scenario = loadScenario(example("/examples/challenges/market_making.toml"));
    constexpr std::size_t kMarkets = 100;
    for (auto _ : state) {
        const std::int64_t before = residentBytes();
        std::vector<std::unique_ptr<Gateway>> markets;
        std::vector<ConnectionId> clients;
        for (std::size_t i = 0; i < kMarkets; ++i) {
            markets.push_back(
                std::make_unique<Gateway>(scenario, "", AgentRegistry::withBuiltIns(),
                                          GatewayOptions{}));
            clients.push_back(markets.back()->connect(0));
            markets.back()->receive(clients.back(),
                                    protocol::encode(protocol::Hello{.seat = "you"}), 0);
        }
        for (std::int64_t wall = 0; wall <= 60 * kSecond; wall += 10 * kMillisecond) {
            for (std::size_t i = 0; i < kMarkets; ++i) {
                markets[i]->advance(wall);
                markets[i]->consumeOutput(clients[i],
                                          markets[i]->pendingOutput(clients[i]).size());
            }
        }
        state.counters["bytes_per_market"] =
            static_cast<double>(residentBytes() - before) / static_cast<double>(kMarkets);
    }
}
BENCHMARK(BM_MemoryPerMarket)->Iterations(1)->Unit(benchmark::kMillisecond);

// The example trading day: its opening auction, twelve minutes of trading and its close.
void BM_TradingDay(benchmark::State& state) {
    const Scenario scenario = loadScenario(example("/examples/scenarios/trading_day.toml"));
    for (auto _ : state) {
        benchmark::DoNotOptimize(runScenario(scenario, AgentRegistry::withBuiltIns()));
    }
    state.counters["simulated_s_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * static_cast<double>(scenario.duration / kSecond),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_TradingDay)->Unit(benchmark::kMillisecond);

// Ten minutes of the example prediction market, its probability worked out every second.
void BM_PredictionMarket(benchmark::State& state) {
    Scenario scenario = loadScenario(example("/examples/scenarios/prediction.toml"));
    scenario.duration = 600 * kSecond;
    for (auto _ : state) {
        benchmark::DoNotOptimize(runScenario(scenario, AgentRegistry::withBuiltIns()));
    }
    state.counters["simulated_s_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * 600.0, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_PredictionMarket)->Unit(benchmark::kMillisecond);

// A prediction market's probability at one moment, with news still to come: a sum over how
// much of it, each term a value of the normal distribution.
void BM_PredictionValue(benchmark::State& state) {
    const PredictionConfig config{.probability = 0.5, .newsRate = 0.05, .newsShare = 0.5};
    Timestamp time = 0;
    std::optional<Fundamental> value;
    for (auto _ : state) {
        if (!value || time >= 3'500 * kSecond) {
            value.emplace(config, 3'600 * kSecond, Random{1, 0});
            time = 0;
        }
        time += kSecond;
        benchmark::DoNotOptimize(value->valueAt(time));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PredictionValue);

// The report of five minutes of the playable market with four seats: five replays of it, on one
// thread and on four.
void BM_SessionReport(benchmark::State& state) {
    Session session{.scenario = readText(example("/examples/scenarios/playable.toml")),
                    .seed = 1,
                    .end = 300 * kSecond,
                    .seats = {"ana", "bo", "cy", "di"},
                    .duration = 300 * kSecond};
    const auto threads = static_cast<unsigned>(state.range(0));
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            makeReport(session, AgentRegistry::withBuiltIns(), kSecond, threads));
    }
}
BENCHMARK(BM_SessionReport)->ArgName("threads")->Arg(1)->Arg(4)->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// A minute of the thousand-agent mixed market.
void BM_LargeMarket(benchmark::State& state) {
    Scenario scenario = loadScenario(example("/examples/scenarios/large_market.toml"));
    scenario.duration = 60 * kSecond;
    for (auto _ : state) {
        benchmark::DoNotOptimize(runScenario(scenario, AgentRegistry::withBuiltIns()));
    }
    state.counters["simulated_s_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * 60.0, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_LargeMarket)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace crowdbook
