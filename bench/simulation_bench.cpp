#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>

#include "crowdbook/agents/zero_intelligence.hpp"
#include "crowdbook/simulation.hpp"

namespace crowdbook {
namespace {

constexpr Price kReference = 10'000;
const ZeroIntelligenceConfig kTrader{.limitRate = 2.0, .marketRate = 0.5, .cancelRate = 1.5};
const AgentOptions kOptions{
    .account = {.maxPosition = 1'000, .maxOrderQuantity = 10},
    .latency = {.toExchange = 200 * kMicrosecond,
                .fromExchange = 200 * kMicrosecond,
                .jitter = 50 * kMicrosecond}};

// Trades exactly like the zero-intelligence trader it wraps, but subscribes to the full market
// data stream, so the benchmark measures what streaming costs and nothing else.
class StreamedZeroIntelligence final : public Agent {
public:
    StreamedZeroIntelligence() : inner_(kTrader, kReference) {}

    void onStart(AgentContext& context) override { inner_.onStart(context); }
    void onWakeup(AgentContext& context, std::uint64_t tag) override {
        inner_.onWakeup(context, tag);
    }

private:
    ZeroIntelligenceTrader inner_;
};

// One simulated second of a market with `agents` zero-intelligence traders, each acting about
// four times a second. The counter is simulated seconds per second of wall-clock time: above 1
// the simulation runs faster than real time.
void BM_ZeroIntelligenceMarket(benchmark::State& state) {
    const auto agents = state.range(0);
    const bool streamed = state.range(1) != 0;
    for (auto _ : state) {
        Simulation simulation{1};
        for (std::int64_t i = 0; i < agents; ++i) {
            if (streamed) {
                simulation.addAgent(std::make_unique<StreamedZeroIntelligence>(), kOptions);
            } else {
                simulation.addAgent(std::make_unique<ZeroIntelligenceTrader>(kTrader, kReference),
                                    kOptions);
            }
        }
        simulation.runUntil(kSecond);
        benchmark::DoNotOptimize(simulation.exchange().book().orderCount());
    }
    state.counters["simulated_s_per_s"] =
        benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_ZeroIntelligenceMarket)
    ->ArgNames({"agents", "streamed"})
    ->Args({100, 1})
    ->Args({1'000, 1})
    ->Args({3'000, 1})
    ->Args({100, 0})
    ->Args({1'000, 0})
    ->Args({10'000, 0})
    ->Unit(benchmark::kMillisecond);

} // namespace
} // namespace crowdbook
