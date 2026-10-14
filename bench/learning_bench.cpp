#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "crowdbook/learning.hpp"
#include "crowdbook/random.hpp"

// The learning environment: how many decisions a second a pool of markets steps, by the number of
// markets and of threads, and what starting an episode costs.

namespace crowdbook {
namespace {

std::string exampleMarket() {
    std::ifstream file{std::string{CROWDBOOK_SOURCE_DIR} + "/examples/learning/market.toml"};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// One step of every market of a pool of the example learning market, each taking an action
// drawn at random; a market whose episode ends starts another, so starting episodes counts too.
// The counter is decisions a second, over all the markets.
void BM_LearningPool(benchmark::State& state) {
    const auto markets = static_cast<std::size_t>(state.range(0));
    const auto threads = static_cast<std::size_t>(state.range(1));
    LearningPool pool{exampleMarket(), "learning", markets, threads};
    std::vector<std::uint64_t> seeds(markets);
    for (std::size_t market = 0; market < markets; ++market) {
        seeds[market] = market + 1;
    }
    pool.reset(seeds);
    std::vector<std::int64_t> actions(2 * markets);
    std::vector<std::uint8_t> ended(markets);
    Random random{7, 1};
    for (auto _ : state) {
        for (std::size_t market = 0; market < markets; ++market) {
            const auto action = random.below(kLearningActions.size());
            const auto size = random.below(2);
            actions[2 * market] = static_cast<std::int64_t>(action);
            actions[2 * market + 1] = static_cast<std::int64_t>(size);
        }
        pool.step(actions);
        bool any = false;
        for (std::size_t market = 0; market < markets; ++market) {
            ended[market] = pool.terminated()[market];
            any = any || ended[market] != 0;
            seeds[market] += markets;
        }
        if (any) {
            pool.reset(seeds, {}, ended);
        }
        benchmark::DoNotOptimize(pool.rewards().data());
    }
    state.counters["steps_per_s"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * static_cast<double>(markets),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_LearningPool)
    ->ArgNames({"markets", "threads"})
    ->Args({1, 1})
    ->Args({16, 1})
    ->Args({64, 1})
    ->Args({16, 4})
    ->Args({64, 4})
    ->Args({256, 4})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// Starting an episode: drawing its settings, building its market, and running it to the first
// decision, five seconds in.
void BM_LearningReset(benchmark::State& state) {
    LearningPool pool{exampleMarket(), "learning", 1, 1};
    std::uint64_t seed = 1;
    for (auto _ : state) {
        pool.reset(std::vector<std::uint64_t>{seed++});
        benchmark::DoNotOptimize(pool.state().data());
    }
}
BENCHMARK(BM_LearningReset)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace crowdbook
