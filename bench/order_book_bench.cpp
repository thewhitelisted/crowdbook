#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "crowdbook/order_book.hpp"

namespace crowdbook {
namespace {

constexpr Price kMid = 10'000;

// Draws straight from mt19937_64, whose output the standard fixes exactly, so every machine
// benchmarks the same order stream.
class Random {
public:
    explicit Random(std::uint64_t seed) : rng_(seed) {}

    std::uint64_t below(std::uint64_t bound) { return rng_() % bound; }

    std::int64_t between(std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
    }

private:
    std::mt19937_64 rng_;
};

struct Operation {
    enum class Kind : std::uint8_t { Submit, Cancel };

    Kind kind = Kind::Submit;
    OrderRequest request; // for Submit
    OrderId cancelId = 0; // for Cancel
};

// A mixed stream around a fixed mid price: 50% passive limit orders within 20 ticks of the mid,
// 35% cancels, 10% limit orders that cross the spread and 5% market orders. The stream is built
// by running it through a book, so every cancel targets an order that is resting at that point.
std::vector<Operation> makeMixedFlow(std::size_t count, std::uint64_t seed) {
    Random random{seed};
    OrderBook book;
    std::vector<Fill> fills;
    std::vector<OrderId> rested;
    std::vector<Operation> operations;
    operations.reserve(count);
    OrderId nextId = 1;

    while (operations.size() < count) {
        const std::uint64_t roll = random.below(100);
        if (roll < 35) {
            // Cancel a random order that is still resting, skipping any that have since traded.
            while (!rested.empty()) {
                const auto index = static_cast<std::size_t>(random.below(rested.size()));
                const OrderId id = rested[index];
                rested[index] = rested.back();
                rested.pop_back();
                if (book.cancel(id)) {
                    operations.push_back({.kind = Operation::Kind::Cancel, .cancelId = id});
                    break;
                }
            }
            continue;
        }

        const Side side = random.below(2) == 0 ? Side::Buy : Side::Sell;
        const Price offset = random.between(1, 20);
        OrderRequest request{.id = nextId++,
                             .owner = static_cast<AgentId>(random.below(100)),
                             .side = side,
                             .quantity = random.between(1, 100)};
        if (roll < 85) {
            request.price = side == Side::Buy ? kMid - offset : kMid + offset;
        } else if (roll < 95) {
            request.price = side == Side::Buy ? kMid + offset : kMid - offset;
        } else {
            request.type = OrderType::Market;
        }

        fills.clear();
        if (book.submit(request, fills).status == OrderStatus::Resting) {
            rested.push_back(request.id);
        }
        operations.push_back({.kind = Operation::Kind::Submit, .request = request});
    }
    return operations;
}

void BM_MixedOrderFlow(benchmark::State& state) {
    const std::vector<Operation> operations =
        makeMixedFlow(static_cast<std::size_t>(state.range(0)), 7);
    std::vector<Fill> fills;

    for (auto _ : state) {
        OrderBook book;
        for (const Operation& operation : operations) {
            if (operation.kind == Operation::Kind::Cancel) {
                auto cancelled = book.cancel(operation.cancelId);
                benchmark::DoNotOptimize(cancelled);
            } else {
                fills.clear();
                auto result = book.submit(operation.request, fills);
                benchmark::DoNotOptimize(result);
            }
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_MixedOrderFlow)->Arg(100'000)->Unit(benchmark::kMillisecond);

// Adds passive orders spread over 50 price levels per side, then cancels them all in random
// order. Nothing trades, so this isolates level lookup, queue linking and cancellation.
void BM_AddThenCancel(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    Random random{11};
    std::vector<OrderRequest> orders;
    orders.reserve(count);
    for (OrderId id = 1; id <= count; ++id) {
        const Side side = random.below(2) == 0 ? Side::Buy : Side::Sell;
        const Price offset = random.between(1, 50);
        orders.push_back({.id = id,
                          .owner = static_cast<AgentId>(random.below(100)),
                          .side = side,
                          .price = side == Side::Buy ? kMid - offset : kMid + offset,
                          .quantity = random.between(1, 100)});
    }
    std::vector<OrderId> cancelOrder(count);
    std::iota(cancelOrder.begin(), cancelOrder.end(), OrderId{1});
    for (std::size_t i = count - 1; i > 0; --i) {
        std::swap(cancelOrder[i], cancelOrder[static_cast<std::size_t>(random.below(i + 1))]);
    }

    std::vector<Fill> fills;
    for (auto _ : state) {
        OrderBook book;
        for (const OrderRequest& order : orders) {
            auto result = book.submit(order, fills);
            benchmark::DoNotOptimize(result);
        }
        for (const OrderId id : cancelOrder) {
            auto cancelled = book.cancel(id);
            benchmark::DoNotOptimize(cancelled);
        }
    }
    state.SetItemsProcessed(state.iterations() * 2 * state.range(0));
}
BENCHMARK(BM_AddThenCancel)->Arg(100'000)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace crowdbook
