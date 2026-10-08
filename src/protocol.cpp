#include "crowdbook/protocol.hpp"

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <utility>

#include "json.hpp"
#include "overloaded.hpp"

namespace crowdbook::protocol {

namespace {

constexpr std::int64_t kMaxInt = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMinInt = std::numeric_limits<std::int64_t>::min();

constexpr std::array kSides{std::pair{std::string_view{"buy"}, Side::Buy},
                            std::pair{std::string_view{"sell"}, Side::Sell}};
constexpr std::array kOrderTypes{std::pair{std::string_view{"limit"}, OrderType::Limit},
                                 std::pair{std::string_view{"market"}, OrderType::Market}};
constexpr std::array kTimesInForce{
    std::pair{std::string_view{"good-till-cancel"}, TimeInForce::GoodTillCancel},
    std::pair{std::string_view{"immediate-or-cancel"}, TimeInForce::ImmediateOrCancel},
    std::pair{std::string_view{"post-only"}, TimeInForce::PostOnly}};
constexpr std::array kRequestKinds{std::pair{std::string_view{"new"}, RequestKind::New},
                                   std::pair{std::string_view{"cancel"}, RequestKind::Cancel},
                                   std::pair{std::string_view{"modify"}, RequestKind::Modify}};
constexpr std::array kRejectReasons{
    std::pair{std::string_view{"non-positive-quantity"}, RejectReason::NonPositiveQuantity},
    std::pair{std::string_view{"invalid-price"}, RejectReason::InvalidPrice},
    std::pair{std::string_view{"order-size-limit"}, RejectReason::OrderSizeLimit},
    std::pair{std::string_view{"position-limit"}, RejectReason::PositionLimit},
    std::pair{std::string_view{"duplicate-order-id"}, RejectReason::DuplicateOrderId},
    std::pair{std::string_view{"duplicate-client-order-id"},
              RejectReason::DuplicateClientOrderId},
    std::pair{std::string_view{"unknown-order-id"}, RejectReason::UnknownOrderId},
    std::pair{std::string_view{"unknown-agent"}, RejectReason::UnknownAgent},
    std::pair{std::string_view{"post-only-would-trade"}, RejectReason::PostOnlyWouldTrade},
    std::pair{std::string_view{"loss-limit"}, RejectReason::LossLimit},
    std::pair{std::string_view{"rate-limit"}, RejectReason::RateLimit},
    std::pair{std::string_view{"auction-order-type"}, RejectReason::AuctionOrderType},
    std::pair{std::string_view{"market-closed"}, RejectReason::MarketClosed}};
constexpr std::array kPhases{
    std::pair{std::string_view{"continuous"}, Phase::Continuous},
    std::pair{std::string_view{"opening-auction"}, Phase::OpeningAuction},
    std::pair{std::string_view{"halt"}, Phase::HaltAuction},
    std::pair{std::string_view{"closing-auction"}, Phase::ClosingAuction},
    std::pair{std::string_view{"closed"}, Phase::Closed}};
constexpr std::array kCancelReasons{
    std::pair{std::string_view{"requested"}, CancelReason::Requested},
    std::pair{std::string_view{"immediate-or-cancel"}, CancelReason::ImmediateOrCancel},
    std::pair{std::string_view{"self-trade"}, CancelReason::SelfTrade}};
constexpr std::array kMarks{std::pair{std::string_view{"last"}, Mark::LastTrade},
                            std::pair{std::string_view{"value"}, Mark::Value}};
constexpr std::array kBenchmarks{std::pair{std::string_view{"vwap"}, Benchmark::Vwap},
                                 std::pair{std::string_view{"reference"}, Benchmark::Reference}};
constexpr std::array kLiquidities{std::pair{std::string_view{"maker"}, Liquidity::Maker},
                                  std::pair{std::string_view{"taker"}, Liquidity::Taker},
                                  std::pair{std::string_view{"auction"}, Liquidity::Auction}};

template <typename Enum, std::size_t N>
std::string_view nameOf(Enum value,
                        const std::array<std::pair<std::string_view, Enum>, N>& names) {
    for (const auto& [name, candidate] : names) {
        if (candidate == value) {
            return name;
        }
    }
    throw std::invalid_argument("a value with no name on the wire");
}

// Writes one JSON object, field by field.
class ObjectWriter {
public:
    explicit ObjectWriter(std::string& out) : out_(out) { out_ += '{'; }

    ObjectWriter& field(std::string_view key, std::int64_t value) {
        key_(key);
        out_ += std::format("{}", value);
        return *this;
    }
    ObjectWriter& field(std::string_view key, std::uint64_t value) {
        return field(key, static_cast<std::int64_t>(value));
    }
    ObjectWriter& field(std::string_view key, bool value) {
        key_(key);
        out_ += value ? "true" : "false";
        return *this;
    }
    ObjectWriter& field(std::string_view key, std::string_view value) {
        key_(key);
        json::appendString(out_, value);
        return *this;
    }
    ObjectWriter& field(std::string_view key, const char* value) {
        return field(key, std::string_view{value});
    }
    // A field whose value the caller writes into out() next.
    std::string& open(std::string_view key) {
        key_(key);
        return out_;
    }
    void close() { out_ += '}'; }

private:
    void key_(std::string_view key) {
        if (!first_) {
            out_ += ',';
        }
        first_ = false;
        json::appendString(out_, key);
        out_ += ':';
    }

    std::string& out_;
    bool first_ = true;
};

void writeLevel(std::string& out, const LevelSummary& level) {
    ObjectWriter{out}
        .field("price", level.price)
        .field("quantity", level.quantity)
        .field("orders", static_cast<std::uint64_t>(level.orderCount))
        .close();
}

void writeLevels(std::string& out, const std::vector<LevelSummary>& levels) {
    out += '[';
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        writeLevel(out, levels[i]);
    }
    out += ']';
}

void writeSide(std::string& out, const std::optional<LevelSummary>& level) {
    if (level) {
        writeLevel(out, *level);
    } else {
        out += "null";
    }
}

void writeOrder(std::string& out, const OwnOrder& order) {
    ObjectWriter{out}
        .field("id", order.clientOrderId)
        .field("order_id", order.orderId)
        .field("side", nameOf(order.side, kSides))
        .field("order_type", nameOf(order.type, kOrderTypes))
        .field("price", order.price)
        .field("leaves", order.leaves)
        .field("acknowledged", order.acknowledged)
        .field("cancel_requested", order.cancelRequested)
        .close();
}

// Writes the event's type, then the time it reached the seat, then its own fields.
void writeChallenge(std::string& out, const std::optional<ChallengeInfo>& challenge) {
    if (!challenge) {
        out += "null";
        return;
    }
    ObjectWriter{out}.field("name", challenge->name).field("briefing", challenge->briefing).close();
}

void writeScoring(std::string& out, const std::optional<ScoringConfig>& scoring) {
    if (!scoring) {
        out += "null";
        return;
    }
    ObjectWriter writer{out};
    writer.field("mark", nameOf(scoring->mark, kMarks))
        .field("inventory_penalty", scoring->inventoryPenalty)
        .field("close_penalty", scoring->closePenalty)
        .field("max_loss", scoring->maxLoss);
    std::string& target = writer.open("target");
    if (const auto& t = scoring->target) {
        ObjectWriter{target}
            .field("side", nameOf(t->side, kSides))
            .field("quantity", t->quantity)
            .field("benchmark", nameOf(t->benchmark, kBenchmarks))
            .field("unfinished_penalty", t->unfinishedPenalty)
            .close();
    } else {
        target += "null";
    }
    writer.close();
}

void writeScore(std::string& out, const std::optional<Score>& score) {
    if (!score) {
        out += "null";
        return;
    }
    ObjectWriter writer{out};
    writer.field("total", score->total)
        .field("pnl", score->pnl)
        .field("inventory", score->inventory)
        .field("close", score->close)
        .field("paper", score->paper)
        .field("unfinished", score->unfinished)
        .field("unfinished_lots", score->unfinishedLots);
    std::string& stopped = writer.open("stopped_at");
    stopped += score->stoppedAt ? std::format("{}", *score->stoppedAt) : "null";
    writer.close();
}

void writeEvent(ObjectWriter& writer, Timestamp time, const Event& event) {
    const auto head = [&](std::string_view type) -> ObjectWriter& {
        return writer.field("type", type).field("time", time);
    };
    std::visit(
        detail::Overloaded{
            [&](const OrderAccepted& e) {
                head("accepted")
                    .field("id", e.clientOrderId)
                    .field("order_id", e.orderId)
                    .field("side", nameOf(e.side, kSides))
                    .field("order_type", nameOf(e.type, kOrderTypes));
                if (e.type == OrderType::Limit) {
                    writer.field("time_in_force", nameOf(e.timeInForce, kTimesInForce))
                        .field("price", e.price);
                }
                writer.field("quantity", e.quantity);
            },
            [&](const OrderRejected& e) {
                head("rejected")
                    .field("id", e.clientOrderId)
                    .field("request", nameOf(e.request, kRequestKinds))
                    .field("reason", nameOf(e.reason, kRejectReasons));
            },
            [&](const OrderModified& e) {
                head("modified")
                    .field("id", e.clientOrderId)
                    .field("order_id", e.orderId)
                    .field("price", e.price)
                    .field("quantity", e.quantity);
            },
            [&](const OrderFilled& e) {
                head("filled")
                    .field("id", e.clientOrderId)
                    .field("order_id", e.orderId)
                    .field("side", nameOf(e.side, kSides))
                    .field("price", e.price)
                    .field("quantity", e.quantity)
                    .field("leaves", e.leavesQuantity)
                    .field("liquidity", nameOf(e.liquidity, kLiquidities))
                    .field("fee", e.fee);
            },
            [&](const OrderCancelled& e) {
                head("cancelled")
                    .field("id", e.clientOrderId)
                    .field("order_id", e.orderId)
                    .field("quantity", e.quantity)
                    .field("reason", nameOf(e.reason, kCancelReasons));
            },
            [&](const Trade& e) {
                head("trade")
                    .field("price", e.price)
                    .field("quantity", e.quantity)
                    .field("aggressor", nameOf(e.aggressorSide, kSides));
                if (e.auction) {
                    writer.field("auction", true);
                }
            },
            [&](const PhaseChanged& e) {
                head("phase").field("phase", nameOf(e.phase, kPhases));
                std::string& price = writer.open("price");
                price += e.price ? std::format("{}", *e.price) : "null";
            },
            [&](const Indicative& e) {
                head("indicative");
                std::string& price = writer.open("price");
                price += e.uncross ? std::format("{}", e.uncross->price) : "null";
                writer.field("volume", e.uncross ? e.uncross->volume : 0)
                    .field("imbalance", e.uncross ? e.uncross->imbalance : 0);
            },
            [&](const TopOfBook& e) {
                head("top");
                writeSide(writer.open("bid"), e.bid);
                writeSide(writer.open("ask"), e.ask);
            },
            [&](const BookDepth& e) {
                head("depth");
                writeLevels(writer.open("bids"), e.bids);
                writeLevels(writer.open("asks"), e.asks);
            },
        },
        event);
}

// Reads the fields of one JSON object. Every field has to be read, so an unknown one is an error.
class Fields {
public:
    Fields(const json::Value& value, std::string_view what) : what_(what) {
        object_ = std::get_if<json::Object>(&value.data);
        if (object_ == nullptr) {
            throw ProtocolError(std::format("{} must be an object", what_));
        }
        used_.assign(object_->size(), false);
    }

    [[nodiscard]] const json::Value* find(std::string_view key) {
        for (std::size_t i = 0; i < object_->size(); ++i) {
            if ((*object_)[i].key == key) {
                used_[i] = true;
                return &(*object_)[i].value;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has(std::string_view key) const {
        for (const json::Member& member : *object_) {
            if (member.key == key) {
                return true;
            }
        }
        return false;
    }

    const json::Value& required(std::string_view key) {
        const json::Value* value = find(key);
        if (value == nullptr) {
            throw ProtocolError(std::format("missing field '{}'", key));
        }
        return *value;
    }

    std::int64_t integer(std::string_view key, std::int64_t min = kMinInt,
                         std::int64_t max = kMaxInt) {
        return integerOf(required(key), key, min, max);
    }

    static std::int64_t integerOf(const json::Value& value, std::string_view key,
                                  std::int64_t min, std::int64_t max) {
        const auto* number = std::get_if<std::int64_t>(&value.data);
        if (number == nullptr) {
            throw ProtocolError(std::format("'{}' must be a whole number", key));
        }
        if (*number < min || *number > max) {
            throw ProtocolError(
                std::format("'{}' must be between {} and {}, not {}", key, min, max, *number));
        }
        return *number;
    }

    std::string text(std::string_view key) {
        const auto* value = std::get_if<std::string>(&required(key).data);
        if (value == nullptr) {
            throw ProtocolError(std::format("'{}' must be a string", key));
        }
        return *value;
    }

    bool boolean(std::string_view key) {
        const auto* value = std::get_if<bool>(&required(key).data);
        if (value == nullptr) {
            throw ProtocolError(std::format("'{}' must be true or false", key));
        }
        return *value;
    }

    template <typename Enum, std::size_t N>
    Enum choice(std::string_view key,
                const std::array<std::pair<std::string_view, Enum>, N>& names) {
        const std::string value = text(key);
        for (const auto& [name, result] : names) {
            if (name == value) {
                return result;
            }
        }
        throw ProtocolError(std::format("'{}' cannot be '{}'", key, value));
    }

    const json::Array& array(std::string_view key) {
        const auto* value = std::get_if<json::Array>(&required(key).data);
        if (value == nullptr) {
            throw ProtocolError(std::format("'{}' must be a list", key));
        }
        return *value;
    }

    // Throws for any field not read.
    void finish() const {
        for (std::size_t i = 0; i < object_->size(); ++i) {
            if (!used_[i]) {
                throw ProtocolError(
                    std::format("unknown field '{}' in {}", (*object_)[i].key, what_));
            }
        }
    }

private:
    std::string_view what_;
    const json::Object* object_ = nullptr;
    std::vector<bool> used_;
};

json::Value parseLine(std::string_view line) {
    if (line.ends_with('\n')) {
        line.remove_suffix(1);
    }
    try {
        return json::parse(line);
    } catch (const json::ParseError& error) {
        throw ProtocolError(std::format("not valid JSON: {}", error.what()));
    }
}

std::size_t count(Fields& fields, std::string_view key) {
    return static_cast<std::size_t>(fields.integer(key, 0));
}

LevelSummary readLevel(const json::Value& value) {
    Fields fields{value, "a price level"};
    LevelSummary level{.price = fields.integer("price"),
                       .quantity = fields.integer("quantity"),
                       .orderCount = count(fields, "orders")};
    fields.finish();
    return level;
}

std::optional<LevelSummary> readSide(Fields& fields, std::string_view key) {
    const json::Value& value = fields.required(key);
    if (std::holds_alternative<std::nullptr_t>(value.data)) {
        return std::nullopt;
    }
    return readLevel(value);
}

std::vector<LevelSummary> readLevels(Fields& fields, std::string_view key) {
    std::vector<LevelSummary> levels;
    for (const json::Value& value : fields.array(key)) {
        levels.push_back(readLevel(value));
    }
    return levels;
}

ClientOrderId wireId(Fields& fields) {
    return static_cast<ClientOrderId>(fields.integer("id", 1));
}

OwnOrder readOrder(const json::Value& value) {
    Fields fields{value, "an order"};
    OwnOrder order{.clientOrderId = wireId(fields),
                   .orderId = static_cast<OrderId>(fields.integer("order_id", 0)),
                   .side = fields.choice("side", kSides),
                   .type = fields.choice("order_type", kOrderTypes),
                   .price = fields.integer("price"),
                   .leaves = fields.integer("leaves"),
                   .acknowledged = fields.boolean("acknowledged"),
                   .cancelRequested = fields.boolean("cancel_requested")};
    fields.finish();
    return order;
}

Latency readLatency(const json::Value& value) {
    Fields fields{value, "latency"};
    Latency latency{.toExchange = fields.integer("to_exchange", 0),
                    .fromExchange = fields.integer("from_exchange", 0),
                    .jitter = fields.integer("jitter", 0)};
    fields.finish();
    return latency;
}

AccountState readAccount(const json::Value& value) {
    Fields fields{value, "account"};
    AccountState account{.initialCash = fields.integer("initial_cash"),
                         .initialPosition = fields.integer("initial_position"),
                         .cash = fields.integer("cash"),
                         .position = fields.integer("position"),
                         .fees = fields.integer("fees"),
                         .maxPosition = fields.integer("max_position", 0),
                         .maxOrderQuantity = fields.integer("max_order_quantity", 0)};
    fields.finish();
    return account;
}

bool isNull(const json::Value& value) {
    return std::holds_alternative<std::nullptr_t>(value.data);
}

std::optional<ChallengeInfo> readChallenge(const json::Value& value) {
    if (isNull(value)) {
        return std::nullopt;
    }
    Fields fields{value, "challenge"};
    ChallengeInfo challenge{.name = fields.text("name"), .briefing = fields.text("briefing")};
    fields.finish();
    return challenge;
}

std::optional<ScoringConfig> readScoring(const json::Value& value) {
    if (isNull(value)) {
        return std::nullopt;
    }
    Fields fields{value, "scoring"};
    ScoringConfig scoring{.mark = fields.choice("mark", kMarks),
                          .inventoryPenalty = fields.integer("inventory_penalty", 0),
                          .closePenalty = fields.integer("close_penalty", 0),
                          .maxLoss = fields.integer("max_loss", 0)};
    if (const json::Value& target = fields.required("target"); !isNull(target)) {
        Fields inner{target, "target"};
        scoring.target = Target{.side = inner.choice("side", kSides),
                                .quantity = inner.integer("quantity", 1),
                                .benchmark = inner.choice("benchmark", kBenchmarks),
                                .unfinishedPenalty = inner.integer("unfinished_penalty", 0)};
        inner.finish();
    }
    fields.finish();
    return scoring;
}

std::optional<Score> readScore(const json::Value& value) {
    if (isNull(value)) {
        return std::nullopt;
    }
    Fields fields{value, "score"};
    Score score;
    score.total = fields.integer("total");
    score.pnl = fields.integer("pnl");
    score.inventory = fields.integer("inventory");
    score.close = fields.integer("close");
    score.paper = fields.integer("paper");
    score.unfinished = fields.integer("unfinished");
    score.unfinishedLots = fields.integer("unfinished_lots", 0);
    if (const json::Value& stopped = fields.required("stopped_at"); !isNull(stopped)) {
        score.stoppedAt = Fields::integerOf(stopped, "stopped_at", 0, kMaxInt);
    }
    fields.finish();
    return score;
}

Welcome readWelcome(Fields& fields) {
    Welcome welcome{.protocol = fields.integer("protocol"),
                    .seat = fields.text("seat"),
                    .started = fields.boolean("started"),
                    .time = fields.integer("time", 0),
                    .duration = fields.integer("duration", 0),
                    .referencePrice = fields.integer("reference_price"),
                    .depthLevels = count(fields, "depth_levels"),
                    .makerFee = fields.integer("maker_fee"),
                    .takerFee = fields.integer("taker_fee"),
                    .auctionFee = fields.integer("auction_fee", 0),
                    .phase = fields.choice("phase", kPhases),
                    .latency = readLatency(fields.required("latency")),
                    .account = readAccount(fields.required("account"))};
    for (const json::Value& value : fields.array("orders")) {
        welcome.orders.push_back(readOrder(value));
    }
    welcome.challenge = readChallenge(fields.required("challenge"));
    welcome.scoring = readScoring(fields.required("scoring"));
    return welcome;
}

Event readEvent(std::string_view type, Fields& fields) {
    if (type == "accepted") {
        OrderAccepted event{.clientOrderId = wireId(fields),
                            .orderId = static_cast<OrderId>(fields.integer("order_id", 0)),
                            .side = fields.choice("side", kSides),
                            .type = fields.choice("order_type", kOrderTypes)};
        if (event.type == OrderType::Limit) {
            event.timeInForce = fields.choice("time_in_force", kTimesInForce);
            event.price = fields.integer("price");
        }
        event.quantity = fields.integer("quantity");
        return event;
    }
    if (type == "rejected") {
        return OrderRejected{.clientOrderId = wireId(fields),
                             .request = fields.choice("request", kRequestKinds),
                             .reason = fields.choice("reason", kRejectReasons)};
    }
    if (type == "modified") {
        return OrderModified{.clientOrderId = wireId(fields),
                             .orderId = static_cast<OrderId>(fields.integer("order_id", 0)),
                             .price = fields.integer("price"),
                             .quantity = fields.integer("quantity")};
    }
    if (type == "filled") {
        return OrderFilled{.clientOrderId = wireId(fields),
                           .orderId = static_cast<OrderId>(fields.integer("order_id", 0)),
                           .side = fields.choice("side", kSides),
                           .price = fields.integer("price"),
                           .quantity = fields.integer("quantity"),
                           .leavesQuantity = fields.integer("leaves"),
                           .liquidity = fields.choice("liquidity", kLiquidities),
                           .fee = fields.integer("fee")};
    }
    if (type == "cancelled") {
        return OrderCancelled{.clientOrderId = wireId(fields),
                              .orderId = static_cast<OrderId>(fields.integer("order_id", 0)),
                              .quantity = fields.integer("quantity"),
                              .reason = fields.choice("reason", kCancelReasons)};
    }
    if (type == "trade") {
        Trade trade{.price = fields.integer("price"),
                    .quantity = fields.integer("quantity"),
                    .aggressorSide = fields.choice("aggressor", kSides)};
        if (fields.has("auction")) {
            trade.auction = fields.boolean("auction");
        }
        return trade;
    }
    if (type == "phase") {
        PhaseChanged phase{.phase = fields.choice("phase", kPhases)};
        if (const json::Value& price = fields.required("price"); !isNull(price)) {
            phase.price = Fields::integerOf(price, "price", kMinInt, kMaxInt);
        }
        return phase;
    }
    if (type == "indicative") {
        Indicative indicative;
        const json::Value& price = fields.required("price");
        const Quantity volume = fields.integer("volume");
        const Quantity imbalance = fields.integer("imbalance");
        if (!isNull(price)) {
            indicative.uncross = Uncross{.price = Fields::integerOf(price, "price", kMinInt, kMaxInt),
                                         .volume = volume,
                                         .imbalance = imbalance};
        }
        return indicative;
    }
    if (type == "top") {
        TopOfBook top;
        top.bid = readSide(fields, "bid");
        top.ask = readSide(fields, "ask");
        return top;
    }
    if (type == "depth") {
        BookDepth depth;
        depth.bids = readLevels(fields, "bids");
        depth.asks = readLevels(fields, "asks");
        return depth;
    }
    throw ProtocolError(std::format("unknown message type '{}'", type));
}

bool printable(std::string_view text) noexcept {
    for (const char c : text) {
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

} // namespace

bool validSeat(std::string_view seat) noexcept {
    if (seat.empty() || seat.size() > kMaxSeatLength) {
        return false;
    }
    for (const char c : seat) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string wireName(RejectReason reason) {
    return std::string{nameOf(reason, kRejectReasons)};
}

std::string encode(const ClientMessage& message) {
    std::string out;
    ObjectWriter writer{out};
    std::visit(detail::Overloaded{
                   [&](const Hello& m) {
                       writer.field("type", "hello")
                           .field("protocol", m.protocol)
                           .field("seat", m.seat);
                       if (!m.token.empty()) {
                           writer.field("token", m.token);
                       }
                   },
                   [&](const NewOrder& m) {
                       writer.field("type", "new")
                           .field("id", m.clientOrderId)
                           .field("side", nameOf(m.side, kSides))
                           .field("order_type", nameOf(m.type, kOrderTypes));
                       if (m.type == OrderType::Limit) {
                           writer.field("time_in_force", nameOf(m.timeInForce, kTimesInForce))
                               .field("price", m.price);
                       }
                       writer.field("quantity", m.quantity);
                       if (m.parent != 0) {
                           writer.field("parent", m.parent);
                       }
                   },
                   [&](const CancelOrder& m) {
                       writer.field("type", "cancel").field("id", m.clientOrderId);
                   },
                   [&](const ModifyOrder& m) {
                       writer.field("type", "modify")
                           .field("id", m.clientOrderId)
                           .field("price", m.price)
                           .field("quantity", m.quantity);
                   },
               },
               message);
    writer.close();
    out += '\n';
    return out;
}

std::string encode(const ServerMessage& message) {
    std::string out;
    ObjectWriter writer{out};
    std::visit(detail::Overloaded{
                   [&](const Welcome& m) {
                       writer.field("type", "welcome")
                           .field("protocol", m.protocol)
                           .field("seat", m.seat)
                           .field("started", m.started)
                           .field("time", m.time)
                           .field("duration", m.duration)
                           .field("reference_price", m.referencePrice)
                           .field("depth_levels", static_cast<std::uint64_t>(m.depthLevels))
                           .field("maker_fee", m.makerFee)
                           .field("taker_fee", m.takerFee)
                           .field("auction_fee", m.auctionFee)
                           .field("phase", nameOf(m.phase, kPhases));
                       ObjectWriter{writer.open("latency")}
                           .field("to_exchange", m.latency.toExchange)
                           .field("from_exchange", m.latency.fromExchange)
                           .field("jitter", m.latency.jitter)
                           .close();
                       ObjectWriter{writer.open("account")}
                           .field("initial_cash", m.account.initialCash)
                           .field("initial_position", m.account.initialPosition)
                           .field("cash", m.account.cash)
                           .field("position", m.account.position)
                           .field("fees", m.account.fees)
                           .field("max_position", m.account.maxPosition)
                           .field("max_order_quantity", m.account.maxOrderQuantity)
                           .close();
                       std::string& orders = writer.open("orders");
                       orders += '[';
                       for (std::size_t i = 0; i < m.orders.size(); ++i) {
                           if (i > 0) {
                               orders += ',';
                           }
                           writeOrder(orders, m.orders[i]);
                       }
                       orders += ']';
                       writeChallenge(writer.open("challenge"), m.challenge);
                       writeScoring(writer.open("scoring"), m.scoring);
                   },
                   [&](const Start& m) { writer.field("type", "start").field("time", m.time); },
                   [&](const Clock& m) { writer.field("type", "clock").field("time", m.time); },
                   [&](const MarketMessage& m) { writeEvent(writer, m.time, m.event); },
                   [&](const End& m) {
                       writer.field("type", "end")
                           .field("time", m.time)
                           .field("cash", m.cash)
                           .field("position", m.position)
                           .field("fees", m.fees)
                           .field("pnl", m.pnl);
                       writeScore(writer.open("score"), m.score);
                   },
                   [&](const Error& m) {
                       writer.field("type", "error")
                           .field("message", m.message)
                           .field("fatal", m.fatal);
                   },
               },
               message);
    out += '}';
    out += '\n';
    return out;
}

ClientMessage decodeClient(std::string_view line) {
    const json::Value value = parseLine(line);
    Fields fields{value, "a message"};
    const std::string type = fields.text("type");
    ClientMessage message;
    if (type == "hello") {
        Hello hello{.protocol = fields.integer("protocol"), .seat = fields.text("seat")};
        if (!validSeat(hello.seat)) {
            throw ProtocolError(std::format(
                "a seat name has 1 to {} letters, digits, '-' or '_'", kMaxSeatLength));
        }
        if (fields.has("token")) {
            hello.token = fields.text("token");
            if (hello.token.empty() || hello.token.size() > kMaxTokenLength ||
                !printable(hello.token)) {
                throw ProtocolError(std::format(
                    "a token has 1 to {} printable characters and no spaces", kMaxTokenLength));
            }
        }
        message = std::move(hello);
    } else if (type == "new") {
        NewOrder order{.clientOrderId = wireId(fields),
                       .side = fields.choice("side", kSides),
                       .type = fields.choice("order_type", kOrderTypes)};
        if (order.type == OrderType::Limit) {
            order.timeInForce = fields.choice("time_in_force", kTimesInForce);
            order.price = fields.integer("price");
        }
        order.quantity = fields.integer("quantity");
        if (fields.has("parent")) {
            order.parent = static_cast<std::uint64_t>(fields.integer("parent", 0));
        }
        message = order;
    } else if (type == "cancel") {
        message = CancelOrder{.clientOrderId = wireId(fields)};
    } else if (type == "modify") {
        message = ModifyOrder{.clientOrderId = wireId(fields),
                              .price = fields.integer("price"),
                              .quantity = fields.integer("quantity")};
    } else {
        throw ProtocolError(std::format("unknown message type '{}'", type));
    }
    fields.finish();
    return message;
}

ServerMessage decodeServer(std::string_view line) {
    const json::Value value = parseLine(line);
    Fields fields{value, "a message"};
    const std::string type = fields.text("type");
    ServerMessage message;
    if (type == "welcome") {
        message = readWelcome(fields);
    } else if (type == "start") {
        message = Start{.time = fields.integer("time", 0)};
    } else if (type == "clock") {
        message = Clock{.time = fields.integer("time", 0)};
    } else if (type == "end") {
        message = End{.time = fields.integer("time", 0),
                      .cash = fields.integer("cash"),
                      .position = fields.integer("position"),
                      .fees = fields.integer("fees"),
                      .pnl = fields.integer("pnl"),
                      .score = readScore(fields.required("score"))};
    } else if (type == "error") {
        message = Error{.message = fields.text("message"), .fatal = fields.boolean("fatal")};
    } else {
        const Timestamp time = fields.integer("time", 0);
        message = MarketMessage{.time = time, .event = readEvent(type, fields)};
    }
    fields.finish();
    return message;
}

} // namespace crowdbook::protocol
