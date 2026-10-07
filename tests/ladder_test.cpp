#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/ledger.hpp"
#include "exchange_test_support.hpp"
#include "ladder.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;

// A two-sided market around 100 with the participant bidding 4 at 98. The screen is filled in
// field by field: GCC 14 at -O3 mistakes the vectors of a braced temporary for uninitialized.
struct Fixture {
    Fixture() {
        ledger.recordRequest(limitOrder(1, Side::Buy, 98, 4));
        screen.now = 65 * kSecond + 300 * kMillisecond;
        screen.end = 600 * kSecond;
        screen.speed = 2.0;
        screen.market.bid = LevelSummary{.price = 99, .quantity = 7};
        screen.market.ask = LevelSummary{.price = 101, .quantity = 3};
        screen.market.lastTrade = 101;
        screen.market.bids.push_back({.price = 99, .quantity = 7});
        screen.market.bids.push_back({.price = 98, .quantity = 12});
        screen.market.asks.push_back({.price = 101, .quantity = 3});
        screen.ledger = &ledger;
        screen.tape.push_back({.time = 1, .trade = {.price = 101, .quantity = 2}});
        screen.referencePrice = 100;
        screen.cursor = 98;
        screen.size = 4;
        screen.message = "hello";
        screen.rows = 20;
        screen.columns = 80;
        screen.color = false;
    }

    Ledger ledger{0, 0};
    ladder::Screen screen;
};

std::string withoutColor(const std::string& line) {
    std::string plain;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\x1b') {
            i = line.find('m', i);
            continue;
        }
        plain += line[i];
    }
    return plain;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// One ladder row, read back from its fixed columns.
struct Row {
    char marker = ' ';
    std::string myBids, bids, price;
    char lastTrade = ' ';
    std::string asks, myAsks;
};

// The ladder row for `price`, if it is shown.
std::optional<Row> rowFor(const std::vector<std::string>& lines, Price price) {
    for (const std::string& colored : lines) {
        const std::string line = withoutColor(colored);
        if (line.size() < 49 || trimmed(line.substr(21, 8)) != std::to_string(price)) {
            continue;
        }
        return Row{.marker = line[0],
                   .myBids = trimmed(line.substr(2, 9)),
                   .bids = trimmed(line.substr(12, 8)),
                   .price = trimmed(line.substr(21, 8)),
                   .lastTrade = line[29],
                   .asks = trimmed(line.substr(31, 8)),
                   .myAsks = trimmed(line.substr(40, 9))};
    }
    return std::nullopt;
}

TEST(LadderTest, CentersOnTheMarketAndFollowsTheCursor) {
    Fixture fixture;
    EXPECT_EQ(ladder::visiblePrices(fixture.screen, 5),
              (std::vector<Price>{102, 101, 100, 99, 98}));
    fixture.screen.cursor = 110;
    EXPECT_EQ(ladder::visiblePrices(fixture.screen, 3), (std::vector<Price>{110, 109, 108}));
    fixture.screen.cursor = 1;
    EXPECT_EQ(ladder::visiblePrices(fixture.screen, 3), (std::vector<Price>{3, 2, 1}));
}

TEST(LadderTest, DrawsTheBookTheParticipantsOrdersAndTheAccount) {
    Fixture fixture;
    const std::vector<std::string> lines = ladder::render(fixture.screen);

    ASSERT_EQ(lines.size(), fixture.screen.rows);
    for (const std::string& line : lines) {
        EXPECT_LE(line.size(), fixture.screen.columns) << line;
    }
    EXPECT_NE(lines[0].find("01:05.3 of 10:00.0  speed 2x  running"), std::string::npos);
    EXPECT_NE(lines[1].find("position +0"), std::string::npos);

    // The cursor's row has the participant's bid beside the book's; the last trade is marked.
    const std::optional<Row> cursor = rowFor(lines, 98);
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(cursor->marker, '>');
    EXPECT_EQ(cursor->myBids, "4");
    EXPECT_EQ(cursor->bids, "12");
    EXPECT_EQ(cursor->asks, "");
    const std::optional<Row> ask = rowFor(lines, 101);
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(ask->lastTrade, '*');
    EXPECT_EQ(ask->asks, "3");
    EXPECT_EQ(ask->marker, ' ');

    EXPECT_NE(lines[lines.size() - 4].find("101 x2 up"), std::string::npos); // the tape
    EXPECT_EQ(lines.back(), "hello");
}

TEST(LadderTest, AScreenConnectedToAServedMarketShowsItsSeat) {
    Fixture fixture;
    fixture.screen.seat = "alice";
    const std::vector<std::string> lines = ladder::render(fixture.screen);
    EXPECT_EQ(lines[0], "crowdbook  01:05.3 of 10:00.0  seat alice");
    EXPECT_EQ(lines[lines.size() - 2], "c cancel here  C cancel all  q quit");
}

TEST(LadderTest, ShowsTheBestPricesWithoutADepthFeed) {
    Fixture fixture;
    fixture.screen.market.bids.clear();
    fixture.screen.market.asks.clear();
    const std::vector<std::string> lines = ladder::render(fixture.screen);
    EXPECT_EQ(rowFor(lines, 99)->bids, "7");
    EXPECT_EQ(rowFor(lines, 98)->bids, ""); // the second level is not published
}

TEST(LadderTest, ColorsAreOnlyEscapeCodesAroundTheSameText) {
    Fixture fixture;
    const std::vector<std::string> plain = ladder::render(fixture.screen);
    fixture.screen.color = true;
    const std::vector<std::string> colored = ladder::render(fixture.screen);
    ASSERT_EQ(plain.size(), colored.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(withoutColor(colored[i]), plain[i]);
    }
    const auto line = [&](Price price) {
        for (const std::string& each : colored) {
            if (withoutColor(each).find(std::to_string(price) + (price == 101 ? "*" : " ")) !=
                std::string::npos) {
                return each;
            }
        }
        return std::string{};
    };
    EXPECT_TRUE(line(98).starts_with("\x1b[7m"));  // the cursor, reversed
    EXPECT_TRUE(line(101).starts_with("\x1b[31m")); // asks in red
    EXPECT_TRUE(line(99).starts_with("\x1b[32m"));  // bids in green
}

} // namespace
} // namespace crowdbook
