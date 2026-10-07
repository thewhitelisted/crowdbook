#include <cstdint>
#include <string>

#include <benchmark/benchmark.h>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

// How many markets one core can run at real time: the challenges, which are markets sized for a
// person to trade in, each with one seat. The counter is simulated seconds per second of
// wall-clock time, so it is also how many such markets one core keeps up with at real time.

namespace crowdbook {
namespace {

const char* const kChallenges[] = {"market_making", "large_order", "news", "informed_flow"};
constexpr Duration kLength = 60 * kSecond;

std::string challenge(std::int64_t index) {
    return std::string{CROWDBOOK_SOURCE_DIR} + "/examples/challenges/" +
           kChallenges[index] + ".toml";
}

// A challenge's market with its seat, run as fast as it goes.
void BM_ChallengeMarket(benchmark::State& state) {
    const Scenario scenario = loadScenario(challenge(state.range(0)));
    for (auto _ : state) {
        SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns());
        market.run.runUntil(kLength);
        benchmark::DoNotOptimize(market.run.simulation().now());
    }
    state.SetLabel(kChallenges[state.range(0)]);
    state.counters["markets_per_core"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * static_cast<double>(kLength / kSecond),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_ChallengeMarket)->DenseRange(0, 3)->Unit(benchmark::kMillisecond)->UseRealTime();

// The same served through the gateway to a client that reads every message: the market, plus
// the market data and order events encoded for the seat, ten milliseconds at a time.
void BM_ServedChallenge(benchmark::State& state) {
    const Scenario scenario = loadScenario(challenge(state.range(0)));
    for (auto _ : state) {
        Gateway gateway{scenario, "", AgentRegistry::withBuiltIns(), {}};
        const ConnectionId client = gateway.connect(0);
        gateway.receive(client, protocol::encode(protocol::Hello{.seat = "you"}), 0);
        for (std::int64_t wall = 0; wall <= kLength; wall += 10 * kMillisecond) {
            gateway.advance(wall);
            gateway.consumeOutput(client, gateway.pendingOutput(client).size());
        }
        benchmark::DoNotOptimize(gateway.now());
    }
    state.SetLabel(kChallenges[state.range(0)]);
    state.counters["markets_per_core"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * static_cast<double>(kLength / kSecond),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_ServedChallenge)->DenseRange(0, 3)->Unit(benchmark::kMillisecond)->UseRealTime();

} // namespace
} // namespace crowdbook
