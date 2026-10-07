#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/order_book.hpp"
#include "order_test_support.hpp"
#include "reference_book.hpp"

namespace crowdbook {
namespace {

using test::ReferenceBook;

constexpr int kStepsPerSeed = 2'000;

struct FlowShape {
    Price lowPrice = 0;
    Price highPrice = 0;
    std::uint64_t owners = 0;
    Quantity maxQuantity = 0;
};

// Narrow price band and few owners: most orders cross; partial fills and self-trades are common.
constexpr FlowShape kCrowded{.lowPrice = 98, .highPrice = 102, .owners = 3, .maxQuantity = 8};
// Wide band and many owners: many price levels and long queues.
constexpr FlowShape kDeep{.lowPrice = 50, .highPrice = 150, .owners = 20, .maxQuantity = 40};

// Draws straight from mt19937_64, whose output the standard fixes exactly (unlike the std
// distributions), so a failing seed reproduces on every platform.
class RandomFlow {
public:
    RandomFlow(std::uint64_t seed, FlowShape shape) : rng_(seed), shape_(shape) {}

    std::uint64_t below(std::uint64_t bound) { return rng_() % bound; }

    std::int64_t between(std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
    }

    Side side() { return below(2) == 0 ? Side::Buy : Side::Sell; }
    Price price() { return between(shape_.lowPrice, shape_.highPrice); }
    Quantity quantity() { return between(1, shape_.maxQuantity); }
    Quantity modifiedQuantity() { return between(0, shape_.maxQuantity); } // 0 is invalid
    AgentId owner() { return static_cast<AgentId>(1 + below(shape_.owners)); }

private:
    std::mt19937_64 rng_;
    FlowShape shape_;
};

void expectSameState(const OrderBook& book, const ReferenceBook& reference) {
    ASSERT_EQ(book.audit(), std::nullopt);
    const std::vector<RestingOrder> resting = reference.orders();
    ASSERT_EQ(book.orderCount(), resting.size());
    for (const RestingOrder& order : resting) {
        ASSERT_EQ(book.find(order.id), order);
    }
    ASSERT_EQ(book.depth(Side::Buy), reference.depth(Side::Buy));
    ASSERT_EQ(book.depth(Side::Sell), reference.depth(Side::Sell));
}

class OrderBookDifferentialTest : public ::testing::TestWithParam<std::uint64_t> {};

// Feeds OrderBook and ReferenceBook the same random submits, cancels and modifies, and requires
// identical results, fills and book state after every step.
TEST_P(OrderBookDifferentialTest, AgreesWithReferenceBook) {
    const std::uint64_t seed = GetParam();
    RandomFlow flow{seed, seed % 2 == 0 ? kCrowded : kDeep};
    OrderBook book;
    ReferenceBook reference;
    std::vector<Fill> bookFills;
    std::vector<Fill> referenceFills;
    OrderId nextId = 1;

    // Usually a resting order; sometimes an id that was filled, cancelled or never used.
    const auto pickId = [&] {
        const std::vector<RestingOrder> resting = reference.orders();
        if (!resting.empty() && flow.below(10) < 8) {
            return resting[flow.below(resting.size())].id;
        }
        return 1 + flow.below(nextId + 2);
    };

    for (int step = 0; step < kStepsPerSeed; ++step) {
        SCOPED_TRACE(std::format("seed {} step {}", seed, step));
        bookFills.clear();
        referenceFills.clear();

        const std::uint64_t action = flow.below(100);
        if (action < 65) {
            OrderRequest request{.id = nextId++,
                                 .owner = flow.owner(),
                                 .side = flow.side(),
                                 .price = flow.price(),
                                 .quantity = flow.quantity()};
            const std::uint64_t variant = flow.below(20);
            if (variant < 3) {
                request.timeInForce = TimeInForce::ImmediateOrCancel;
            } else if (variant < 5) {
                request.type = OrderType::Market;
            } else if (variant == 5) {
                request.id = pickId(); // usually a duplicate
            } else if (variant == 6) {
                request.quantity = -static_cast<Quantity>(flow.below(2)); // zero or negative
            }
            ASSERT_EQ(book.submit(request, bookFills), reference.submit(request, referenceFills));
        } else if (action < 85) {
            const OrderId id = pickId();
            ASSERT_EQ(book.cancel(id), reference.cancel(id));
        } else {
            const OrderId id = pickId();
            const std::optional<RestingOrder> current = reference.find(id);
            const Price price = (current && flow.below(2) == 0) ? current->price : flow.price();
            const Quantity quantity = flow.modifiedQuantity();
            ASSERT_EQ(book.modify(id, price, quantity, bookFills),
                      reference.modify(id, price, quantity, referenceFills));
        }

        ASSERT_EQ(bookFills, referenceFills);
        ASSERT_NO_FATAL_FAILURE(expectSameState(book, reference));
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, OrderBookDifferentialTest,
                         ::testing::Range<std::uint64_t>(1, 41));

} // namespace
} // namespace crowdbook
