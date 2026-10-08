#include "crowdbook/event_log.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

#include "overloaded.hpp"
#include "text.hpp"

namespace crowdbook {

namespace {

// The kinds of row, in the order of kKindNames.
enum class Kind : std::uint8_t {
    New,
    Cancel,
    Modify,
    Accepted,
    Rejected,
    Modified,
    Cancelled,
    Filled,
    Trade,
    TopOfBook,
    Phase,
    Indicative,
};

constexpr std::array<std::string_view, 12> kKindNames = {
    "new",       "cancel", "modify", "accepted",    "rejected", "modified",
    "cancelled", "filled", "trade",  "top_of_book", "phase",    "indicative"};

std::uint32_t bitOf(Kind kind) { return std::uint32_t{1} << static_cast<unsigned>(kind); }

// One CSV row; fields left empty are written as empty columns.
struct Row {
    Timestamp time = 0;
    Kind kind = Kind::New;
    std::optional<std::uint64_t> agent{};
    std::optional<std::uint64_t> clientOrderId{};
    std::optional<std::uint64_t> orderId{};
    std::string_view side{};
    std::string_view type{};
    std::string_view timeInForce{};
    std::optional<Price> price{};
    std::optional<Quantity> quantity{};
    std::optional<Quantity> leaves{};
    std::string_view liquidity{};
    std::optional<Fee> fee{}; // written in tick-lots
    std::string_view request{};
    std::string_view reason{};
    std::optional<Price> bidPrice{};
    std::optional<Quantity> bidQuantity{};
    std::optional<Price> askPrice{};
    std::optional<Quantity> askQuantity{};
    std::optional<std::uint64_t> parent{};
};

template <typename T>
void appendField(std::string& line, const std::optional<T>& value) {
    line += ',';
    if (value) {
        text::appendInteger(line, *value);
    }
}

void appendField(std::string& line, std::string_view value) {
    line += ',';
    line += value;
}

// Writes the row in one call, through `line`, whose capacity carries over from row to row.
void write(std::ostream& out, std::string& line, const Row& row) {
    line.clear();
    text::appendInteger(line, row.time);
    appendField(line, kKindNames[static_cast<std::size_t>(row.kind)]);
    appendField(line, row.agent);
    appendField(line, row.clientOrderId);
    appendField(line, row.orderId);
    appendField(line, row.side);
    appendField(line, row.type);
    appendField(line, row.timeInForce);
    appendField(line, row.price);
    appendField(line, row.quantity);
    appendField(line, row.leaves);
    appendField(line, row.liquidity);
    line += ',';
    if (row.fee) {
        text::appendFee(line, *row.fee);
    }
    appendField(line, row.request);
    appendField(line, row.reason);
    appendField(line, row.bidPrice);
    appendField(line, row.bidQuantity);
    appendField(line, row.askPrice);
    appendField(line, row.askQuantity);
    appendField(line, row.parent);
    line += '\n';
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

// Fills in the order type, and for limit orders the time in force and price.
void describeOrder(Row& row, OrderType type, TimeInForce timeInForce, Price price) {
    row.type = toString(type);
    if (type == OrderType::Limit) {
        row.timeInForce = toString(timeInForce);
        row.price = price;
    }
}

} // namespace

CsvEventLog::CsvEventLog(std::ostream& out, const std::vector<std::string>& kinds) : out_(out) {
    if (!kinds.empty()) {
        keptKinds_ = 0;
    }
    for (const std::string& kind : kinds) {
        const auto* found = std::ranges::find(kKindNames, kind);
        if (found == kKindNames.end()) {
            throw std::invalid_argument(std::format(
                "unknown log row kind '{}'; the kinds are new, cancel, modify, accepted, "
                "rejected, modified, cancelled, filled, trade, top_of_book, phase and "
                "indicative",
                kind));
        }
        keptKinds_ |= bitOf(static_cast<Kind>(found - kKindNames.begin()));
    }
    out_ << "time,kind,agent,client_order_id,order_id,side,type,time_in_force,price,quantity,"
            "leaves,liquidity,fee,request,reason,bid_price,bid_quantity,ask_price,ask_quantity,"
            "parent\n";
}

void CsvEventLog::onRequest(Timestamp time, AgentId agent, const Request& request) {
    Row row{.time = time, .agent = agent, .clientOrderId = clientOrderIdOf(request)};
    if (const auto* order = std::get_if<NewOrder>(&request)) {
        row.kind = Kind::New;
        row.side = toString(order->side);
        describeOrder(row, order->type, order->timeInForce, order->price);
        row.quantity = order->quantity;
        if (order->parent != 0) {
            row.parent = order->parent;
        }
    } else if (const auto* modify = std::get_if<ModifyOrder>(&request)) {
        row.kind = Kind::Modify;
        row.price = modify->price;
        row.quantity = modify->quantity;
    } else {
        row.kind = Kind::Cancel;
    }
    if ((keptKinds_ & bitOf(row.kind)) != 0) {
        write(out_, line_, row);
    }
}

void CsvEventLog::onEvent(Timestamp time, const Event& event) {
    if (std::holds_alternative<BookDepth>(event)) {
        return; // the orders in the log already determine the book; DepthSampler records it
    }
    const Row row = std::visit(
        detail::Overloaded{
            [time](const OrderAccepted& accepted) {
                Row result{.time = time,
                           .kind = Kind::Accepted,
                           .agent = accepted.agent,
                           .clientOrderId = accepted.clientOrderId,
                           .orderId = accepted.orderId,
                           .side = toString(accepted.side),
                           .quantity = accepted.quantity};
                describeOrder(result, accepted.type, accepted.timeInForce, accepted.price);
                return result;
            },
            [time](const OrderRejected& rejected) {
                return Row{.time = time,
                           .kind = Kind::Rejected,
                           .agent = rejected.agent,
                           .clientOrderId = rejected.clientOrderId,
                           .request = toString(rejected.request),
                           .reason = toString(rejected.reason)};
            },
            [time](const OrderModified& modified) {
                return Row{.time = time,
                           .kind = Kind::Modified,
                           .agent = modified.agent,
                           .clientOrderId = modified.clientOrderId,
                           .orderId = modified.orderId,
                           .price = modified.price,
                           .quantity = modified.quantity};
            },
            [time](const OrderFilled& filled) {
                return Row{.time = time,
                           .kind = Kind::Filled,
                           .agent = filled.agent,
                           .clientOrderId = filled.clientOrderId,
                           .orderId = filled.orderId,
                           .side = toString(filled.side),
                           .price = filled.price,
                           .quantity = filled.quantity,
                           .leaves = filled.leavesQuantity,
                           .liquidity = toString(filled.liquidity),
                           .fee = filled.fee};
            },
            [time](const OrderCancelled& cancelled) {
                return Row{.time = time,
                           .kind = Kind::Cancelled,
                           .agent = cancelled.agent,
                           .clientOrderId = cancelled.clientOrderId,
                           .orderId = cancelled.orderId,
                           .quantity = cancelled.quantity,
                           .reason = toString(cancelled.reason)};
            },
            [time](const Trade& trade) {
                return Row{.time = time,
                           .kind = Kind::Trade,
                           .side = toString(trade.aggressorSide),
                           .price = trade.price,
                           .quantity = trade.quantity,
                           .liquidity = trade.auction ? "auction" : ""};
            },
            [time](const PhaseChanged& phase) {
                return Row{.time = time,
                           .kind = Kind::Phase,
                           .price = phase.price,
                           .reason = toString(phase.phase)};
            },
            [time](const Indicative& indicative) {
                Row result{.time = time, .kind = Kind::Indicative};
                if (indicative.uncross) {
                    result.price = indicative.uncross->price;
                    result.quantity = indicative.uncross->volume;
                    result.leaves = indicative.uncross->imbalance;
                }
                return result;
            },
            [time](const TopOfBook& top) {
                Row result{.time = time, .kind = Kind::TopOfBook};
                if (top.bid) {
                    result.bidPrice = top.bid->price;
                    result.bidQuantity = top.bid->quantity;
                }
                if (top.ask) {
                    result.askPrice = top.ask->price;
                    result.askQuantity = top.ask->quantity;
                }
                return result;
            },
            [time](const BookDepth& /*depth*/) { return Row{.time = time}; }, // returned above
        },
        event);
    if ((keptKinds_ & bitOf(row.kind)) != 0) {
        write(out_, line_, row);
    }
}

IntervalSampler::IntervalSampler(Duration interval) : interval_(interval) {
    if (interval <= 0) {
        throw std::invalid_argument("the sampling interval must be positive");
    }
}

void IntervalSampler::onRequest(Timestamp time, AgentId /*agent*/, const Request& /*request*/) {
    writeRowsBefore(time);
}

void IntervalSampler::onEvent(Timestamp time, const Event& event) {
    writeRowsBefore(time);
    update(event);
}

void IntervalSampler::finish(Timestamp end) { writeRowsBefore(end + 1); }

void IntervalSampler::writeRowsBefore(Timestamp time) {
    for (; nextRow_ < time; nextRow_ += interval_) {
        writeRow(nextRow_);
    }
}

namespace {

std::string priceField(const std::optional<Price>& price) {
    return price ? std::to_string(*price) : std::string{};
}

} // namespace

PriceSampler::PriceSampler(std::ostream& out, Duration interval)
    : IntervalSampler(interval), out_(out) {
    out_ << "time,bid,ask,last_trade\n";
}

void PriceSampler::update(const Event& event) {
    if (const auto* trade = std::get_if<Trade>(&event)) {
        market_.lastTrade = trade->price;
    } else if (const auto* top = std::get_if<TopOfBook>(&event)) {
        market_.bid = top->bid;
        market_.ask = top->ask;
    }
}

void PriceSampler::writeRow(Timestamp time) {
    const auto price = [](const std::optional<LevelSummary>& level) {
        return priceField(level ? std::optional{level->price} : std::nullopt);
    };
    out_ << time << ',' << price(market_.bid) << ',' << price(market_.ask) << ','
         << priceField(market_.lastTrade) << '\n';
}

DepthSampler::DepthSampler(std::ostream& out, Duration interval, std::size_t levels)
    : IntervalSampler(interval), out_(out), levels_(levels) {
    if (levels == 0) {
        throw std::invalid_argument("the depth sampler needs at least one level");
    }
    out_ << "time";
    for (const std::string_view side : {"bid", "ask"}) {
        for (std::size_t level = 1; level <= levels; ++level) {
            out_ << std::format(",{0}_price_{1},{0}_quantity_{1}", side, level);
        }
    }
    out_ << '\n';
}

void DepthSampler::update(const Event& event) {
    if (const auto* depth = std::get_if<BookDepth>(&event)) {
        depth_ = *depth;
    }
}

void DepthSampler::writeRow(Timestamp time) {
    out_ << time;
    for (const Levels* side : {&depth_.bids, &depth_.asks}) {
        for (std::size_t level = 0; level < levels_; ++level) {
            if (level < side->size()) {
                out_ << ',' << (*side)[level].price << ',' << (*side)[level].quantity;
            } else {
                out_ << ",,";
            }
        }
    }
    out_ << '\n';
}

void BroadcastSink::onRequest(Timestamp time, AgentId agent, const Request& request) {
    for (EventSink* sink : sinks_) {
        sink->onRequest(time, agent, request);
    }
}

void BroadcastSink::onEvent(Timestamp time, const Event& event) {
    for (EventSink* sink : sinks_) {
        sink->onEvent(time, event);
    }
}

} // namespace crowdbook
