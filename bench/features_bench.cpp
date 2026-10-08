#include <cstdint>
#include <fstream>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>

#include <benchmark/benchmark.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
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
