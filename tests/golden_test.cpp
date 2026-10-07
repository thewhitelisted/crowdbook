#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

// Golden hashes: a fixed market's every output and a recorded session's replay must come out
// byte for byte the same on every platform CI builds on (macOS on arm64 and Linux on x86-64,
// different compilers and standard libraries). A change that is meant to change runs, such as a
// new draw, updates the hashes below; a failure anywhere else means a run depends on the platform.

namespace crowdbook {
namespace {

// FNV-1a, 64 bits: small, and the same everywhere.
std::uint64_t hashOf(std::string_view bytes) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3;
    }
    return hash;
}

std::string hex(std::uint64_t hash) { return std::format("{:#018x}", hash); }

// Every agent type and every source of randomness and floating point: news in a mean-reverting
// value, noise traders that respond to activity and volatility, a post-only market maker paid
// rebates, trend followers, informed and adaptive traders, and brokers working parents both ways.
constexpr std::string_view kMarket = R"(
seed = 2026
duration = "240s"
reference_price = 1000

[fundamental]
volatility = 0.5
mean_reversion = 0.01
step = "100ms"
jump_rate = 0.05
jump_size = 4.0

[exchange]
depth_levels = 5
maker_fee = -0.1
taker_fee = 0.3

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 40
limit_rate = 2.0
market_rate = 0.4
cancel_rate = 0.5
activity_response = 0.8
volatility_response = 1.2
activity_memory = "10s"
activity_baseline = "60s"
latency = { to_exchange = "300us", from_exchange = "300us", jitter = "100us" }

[[agents]]
type = "market_maker"
name = "maker"
intensity = 1.0
post_only = true
latency = { to_exchange = "20us", from_exchange = "20us" }
account = { max_position = 200, max_order_quantity = 10 }

[[agents]]
type = "momentum"
name = "trend"
count = 3
interval = "500ms"
latency = { to_exchange = "500us", from_exchange = "500us", jitter = "200us" }

[[agents]]
type = "informed"
name = "informed"
count = 3
interval = "1s"
latency = { to_exchange = "200us", from_exchange = "200us", jitter = "50us" }

[[agents]]
type = "adaptive"
name = "adaptive"
count = 5
memory = "20s"
latency = { to_exchange = "500us", from_exchange = "500us", jitter = "200us" }

[[agents]]
type = "execution"
name = "twap"
count = 3
min_parent = 10
max_parent = 400
pause = "15s"
interval = "500ms"
child_size = 2
account = { max_position = 100000 }

[[agents]]
type = "execution"
name = "pov"
count = 2
style = "pov"
min_parent = 10
max_parent = 400
pause = "15s"
participation = 0.05
child_size = 4
account = { max_position = 100000 }
)";

constexpr std::uint64_t kMarketLog = 0x0d2b8fee0b0afb38;
constexpr std::uint64_t kMarketPrices = 0xb5dc7ff55dfed436;
constexpr std::uint64_t kMarketDepth = 0x3f64617a1293ca45;
constexpr std::uint64_t kMarketResult = 0xebf5b0556bf4bf09;
constexpr std::uint64_t kDemoReplayLog = 0x8680a02fed133a4e;

TEST(GoldenTest, AMarketsOutputsAreTheSameOnEveryPlatform) {
    const Scenario scenario = parseScenario(kMarket, "golden");
    std::ostringstream log;
    std::ostringstream prices;
    std::ostringstream depth;
    CsvEventLog logSink{log};
    PriceSampler priceSink{prices, kSecond};
    DepthSampler depthSink{depth, kSecond, 5};
    BroadcastSink sinks;
    sinks.add(logSink);
    sinks.add(priceSink);
    sinks.add(depthSink);
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), &sinks);
    priceSink.finish(scenario.duration);
    depthSink.finish(scenario.duration);
    std::ostringstream json;
    writeResultJson(json, scenario, result);

    EXPECT_GT(result.trades, 5'000U); // a busy market, not an empty one
    for (const GroupResult& group : result.groups) {
        EXPECT_GT(group.traded, 0) << group.name; // every agent type took part
    }
    EXPECT_EQ(hex(hashOf(log.str())), hex(kMarketLog));
    EXPECT_EQ(hex(hashOf(prices.str())), hex(kMarketPrices));
    EXPECT_EQ(hex(hashOf(depth.str())), hex(kMarketDepth));
    EXPECT_EQ(hex(hashOf(json.str())), hex(kMarketResult)) << json.str();
}

TEST(GoldenTest, TheDemoSessionReplaysTheSameOnEveryPlatform) {
    const Session session =
        loadSession(std::string{CROWDBOOK_SOURCE_DIR} + "/examples/sessions/playable_demo.toml");
    std::ostringstream log;
    CsvEventLog sink{log};
    static_cast<void>(replaySession(session, AgentRegistry::withBuiltIns(), &sink));
    EXPECT_EQ(hex(hashOf(log.str())), hex(kDemoReplayLog));
}

} // namespace
} // namespace crowdbook
