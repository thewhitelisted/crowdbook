#include "crowdbook/learning.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include <toml++/toml.hpp>

#include "crowdbook/agents/participant.hpp"
#include "crowdbook/live.hpp"
#include "crowdbook/math.hpp"
#include "crowdbook/random.hpp"

#include "overloaded.hpp"
#include "scenario_toml.hpp"

namespace crowdbook {

namespace {

// Random streams with the top bit set, which neither the value (stream 0) nor any agent (2n and
// 2n + 1) can reach. The feed's noise is keyed by the market's seed; each range by the settings
// seed and its path.
constexpr std::uint64_t kTopBit = std::uint64_t{1} << 63;
constexpr std::uint64_t kFeedStream = kTopBit | 1;

// FNV-1a, the same on every platform, unlike std::hash.
std::uint64_t pathStream(std::string_view path) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : path) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash | kTopBit;
}

std::int64_t sign(Side side) noexcept { return side == Side::Buy ? 1 : -1; }

std::size_t index(Side side) noexcept { return side == Side::Buy ? 0 : 1; }

bool hasValue(const Scenario& scenario) noexcept {
    return scenario.fundamental.has_value() || scenario.prediction.has_value();
}

// Whether two configs give observations and actions of the same shape.
bool sameShape(const LearningConfig& a, const LearningConfig& b) noexcept {
    return a.actions == b.actions && a.sizes == b.sizes && a.maxEvents == b.maxEvents &&
           a.maxDepthUpdates == b.maxDepthUpdates && a.depthLevels == b.depthLevels &&
           a.maxOrders == b.maxOrders;
}

// The [[agents]] table a range names by its group's name, or its type if it has none.
toml::table& agentGroup(toml::table& root, std::string_view name, std::string_view path) {
    toml::table* found = nullptr;
    if (auto* agents = root.get_as<toml::array>("agents")) {
        for (toml::node& node : *agents) {
            auto* group = node.as_table();
            if (group == nullptr) {
                continue;
            }
            const auto* named = group->get_as<std::string>("name");
            const auto* type = group->get_as<std::string>("type");
            const std::string_view label = named != nullptr  ? std::string_view{named->get()}
                                           : type != nullptr ? std::string_view{type->get()}
                                                             : std::string_view{};
            if (label == name) {
                if (found != nullptr) {
                    throw ScenarioError(std::format(
                        "range '{}': two agent groups are named '{}'; name them apart", path,
                        name));
                }
                found = group;
            }
        }
    }
    if (found == nullptr) {
        throw ScenarioError(std::format("range '{}': no agent group is named '{}'", path, name));
    }
    return *found;
}

// Sets the range's setting in a scenario document to `value`.
void place(toml::table& root, const SettingRange& range, const Parameters::Value& value) {
    const std::string path = range.path();
    toml::table* table = &root;
    for (std::size_t i = 0; i + 1 < range.keys.size(); ++i) {
        const std::string& key = range.keys[i];
        if (table == &root && key == "agents") {
            if (i + 2 >= range.keys.size()) {
                throw ScenarioError(std::format(
                    "range '{}' must name an agent group and one of its settings", path));
            }
            table = &agentGroup(root, range.keys[++i], path);
            continue;
        }
        toml::table* next = table->get_as<toml::table>(key);
        if (next == nullptr) {
            throw ScenarioError(
                std::format("range '{}': the scenario has no table '{}' to set it in", path, key));
        }
        table = next;
    }
    std::visit([&](const auto& setting) { table->insert_or_assign(range.keys.back(), setting); },
               value);
}

struct DrawnSetting {
    Parameters::Value value{};
    double setting = 0.0; // what the pool reports
};

DrawnSetting drawSetting(const SettingRange& range, std::uint64_t seed) {
    Random random{seed, pathStream(range.path())};
    switch (range.form) {
    case SettingRange::Form::Choices: {
        const auto choice = random.below(range.choices.size());
        const Parameters::Value& value = range.choices[choice];
        double setting = static_cast<double>(choice);
        if (const auto* whole = std::get_if<std::int64_t>(&value)) {
            setting = static_cast<double>(*whole);
        } else if (const auto* number = std::get_if<double>(&value)) {
            setting = *number;
        } else if (const auto* flag = std::get_if<bool>(&value)) {
            setting = *flag ? 1.0 : 0.0;
        }
        return {.value = value, .setting = setting};
    }
    case SettingRange::Form::Log: {
        const double low = math::log(std::get<double>(range.low));
        const double high = math::log(std::get<double>(range.high));
        const double drawn = math::exp(low + (high - low) * random.uniform());
        return {.value = drawn, .setting = drawn};
    }
    case SettingRange::Form::Uniform:
        break;
    }
    // One of low, low + step, ... up to high, for whole numbers: the count of steps drawn.
    const auto stepsOf = [&](std::int64_t low, std::int64_t high, std::int64_t step) {
        return low + random.uniformInt(0, (high - low) / step) * step;
    };
    if (const auto* low = std::get_if<std::int64_t>(&range.low)) {
        const std::int64_t high = std::get<std::int64_t>(range.high);
        const std::int64_t drawn = range.step
                                       ? stepsOf(*low, high, std::get<std::int64_t>(*range.step))
                                       : random.uniformInt(*low, high);
        return {.value = drawn, .setting = static_cast<double>(drawn)};
    }
    if (const auto* low = std::get_if<double>(&range.low)) {
        const double high = std::get<double>(range.high);
        double drawn = 0.0;
        if (range.step) {
            const double step = std::get<double>(*range.step);
            const auto steps = static_cast<std::int64_t>(std::floor((high - *low) / step + 1e-9));
            drawn = *low + static_cast<double>(random.uniformInt(0, steps)) * step;
        } else {
            drawn = *low + (high - *low) * random.uniform();
        }
        return {.value = drawn, .setting = drawn};
    }
    const Duration low = parseDuration(std::get<std::string>(range.low));
    const Duration high = parseDuration(std::get<std::string>(range.high));
    const Duration drawn =
        range.step ? stepsOf(low, high, parseDuration(std::get<std::string>(*range.step)))
                   : random.uniformInt(low, high);
    return {.value = std::format("{}ns", drawn), .setting = static_cast<double>(drawn)};
}

// The highest value a range draws: its last choice, its high end, or the last step below it.
Parameters::Value highestDrawn(const SettingRange& range) {
    if (range.form == SettingRange::Form::Choices) {
        return range.choices.back();
    }
    if (!range.step) {
        return range.high;
    }
    if (const auto* low = std::get_if<std::int64_t>(&range.low)) {
        const std::int64_t step = std::get<std::int64_t>(*range.step);
        return *low + (std::get<std::int64_t>(range.high) - *low) / step * step;
    }
    if (const auto* low = std::get_if<double>(&range.low)) {
        const double step = std::get<double>(*range.step);
        const auto steps = static_cast<std::int64_t>(
            std::floor((std::get<double>(range.high) - *low) / step + 1e-9));
        return *low + static_cast<double>(steps) * step;
    }
    const Duration low = parseDuration(std::get<std::string>(range.low));
    const Duration step = parseDuration(std::get<std::string>(*range.step));
    const Duration high = parseDuration(std::get<std::string>(range.high));
    return std::format("{}ns", low + (high - low) / step * step);
}

// Calls task(i) for each i below a count, on a fixed set of threads and the calling one, and
// returns once every call has. Each call must catch its own exceptions.
class Workers {
public:
    explicit Workers(std::size_t threads) {
        helpers_.reserve(threads > 0 ? threads - 1 : 0);
        for (std::size_t i = 1; i < threads; ++i) {
            helpers_.emplace_back([this] { work(); });
        }
    }
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;
    Workers(Workers&&) = delete;
    Workers& operator=(Workers&&) = delete;
    ~Workers() {
        {
            const std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread& helper : helpers_) {
            helper.join();
        }
    }

    [[nodiscard]] std::size_t threads() const noexcept { return helpers_.size() + 1; }

    void run(std::size_t count, const std::function<void(std::size_t)>& task) {
        if (helpers_.empty() || count < 2) {
            for (std::size_t i = 0; i < count; ++i) {
                task(i);
            }
            return;
        }
        {
            const std::lock_guard lock{mutex_};
            task_ = &task;
            count_ = count;
            next_.store(0);
            busy_ = helpers_.size();
            ++generation_;
        }
        wake_.notify_all();
        drain(task, count);
        std::unique_lock lock{mutex_};
        done_.wait(lock, [this] { return busy_ == 0; });
        task_ = nullptr;
    }

private:
    void drain(const std::function<void(std::size_t)>& task, std::size_t count) {
        for (std::size_t i = next_.fetch_add(1); i < count; i = next_.fetch_add(1)) {
            task(i);
        }
    }

    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(std::size_t)>* task = nullptr;
            std::size_t count = 0;
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) {
                    return;
                }
                seen = generation_;
                task = task_;
                count = count_;
            }
            drain(*task, count);
            {
                const std::lock_guard lock{mutex_};
                if (--busy_ == 0) {
                    done_.notify_one();
                }
            }
        }
    }

    std::vector<std::thread> helpers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(std::size_t)>* task_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0};
    std::size_t busy_ = 0;
    std::uint64_t generation_ = 0;
    bool stopping_ = false;
};

} // namespace

std::string_view toString(LearningEvent event) noexcept {
    switch (event) {
    case LearningEvent::None:
        return "none";
    case LearningEvent::Trade:
        return "trade";
    case LearningEvent::Quote:
        return "quote";
    case LearningEvent::Depth:
        return "depth";
    case LearningEvent::Fill:
        return "fill";
    case LearningEvent::Accepted:
        return "accepted";
    case LearningEvent::Modified:
        return "modified";
    case LearningEvent::Cancelled:
        return "cancelled";
    case LearningEvent::Rejected:
        return "rejected";
    case LearningEvent::Phase:
        return "phase";
    case LearningEvent::Indicative:
        return "indicative";
    case LearningEvent::Feed:
        return "feed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------------------------
// Settings drawn for each episode

struct EpisodeScenarios::Impl {
    std::string text;
    std::string source;
    toml::table root;
    Scenario base;

    [[nodiscard]] const std::vector<SettingRange>& ranges() const noexcept {
        static const std::vector<SettingRange> none;
        return base.learning ? base.learning->ranges : none;
    }

    // The document with every range set by `choose`.
    template <typename Choose>
    [[nodiscard]] toml::table with(Choose&& choose) const {
        toml::table copy = root;
        for (const SettingRange& range : ranges()) {
            place(copy, range, choose(range));
        }
        return copy;
    }
};

EpisodeScenarios::EpisodeScenarios(std::string text, std::string source,
                                   const AgentRegistry& registry, bool check)
    : impl_(std::make_unique<Impl>()) {
    impl_->text = std::move(text);
    impl_->source = std::move(source);
    try {
        impl_->root = toml::parse(impl_->text, impl_->source);
    } catch (const toml::parse_error& error) {
        throw ScenarioError(std::format("{}:{}: {}", impl_->source, error.source().begin.line,
                                        error.description()));
    }
    impl_->base = detail::readScenario(impl_->root, impl_->source);
    if (!check || impl_->ranges().empty()) {
        return;
    }
    // Every range at its low end and at the highest value it draws, then each choice in turn
    // with the rest low.
    const auto build = [&](std::string_view what, const auto& choose) {
        try {
            const Scenario scenario = detail::readScenario(impl_->with(choose), impl_->source);
            if (!sameShape(scenario.learning.value_or(LearningConfig{}),
                           impl_->base.learning.value_or(LearningConfig{}))) {
                throw ScenarioError("a range may not change the actions, the sizes or the "
                                    "observation's capacity");
            }
            const ScenarioRun market{scenario, registry};
        } catch (const ScenarioError& error) {
            throw ScenarioError(
                std::format("{}, with {}: {}", impl_->source, what, error.what()));
        }
    };
    const auto low = [](const SettingRange& range) {
        return range.form == SettingRange::Form::Choices ? range.choices.front() : range.low;
    };
    build("every range at its low end", low);
    build("every range at its highest", highestDrawn);
    for (const SettingRange& varied : impl_->ranges()) {
        for (std::size_t i = 1; i + 1 < varied.choices.size(); ++i) {
            build(std::format("'{}' at choice {}", varied.path(), i + 1),
                  [&](const SettingRange& range) {
                      return &range == &varied ? range.choices[i] : low(range);
                  });
        }
    }
    // And a few draws, for settings that take only some of the values between the ends.
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        static_cast<void>(draw(seed));
    }
}

EpisodeScenarios::EpisodeScenarios(EpisodeScenarios&&) noexcept = default;
EpisodeScenarios& EpisodeScenarios::operator=(EpisodeScenarios&&) noexcept = default;
EpisodeScenarios::~EpisodeScenarios() = default;

EpisodeScenarios::Drawn EpisodeScenarios::draw(std::uint64_t settingsSeed) const {
    if (impl_->ranges().empty()) {
        return {.scenario = impl_->base, .settings = {}};
    }
    Drawn drawn;
    drawn.settings.reserve(impl_->ranges().size());
    const toml::table document = impl_->with([&](const SettingRange& range) {
        DrawnSetting setting = drawSetting(range, settingsSeed);
        drawn.settings.push_back(setting.setting);
        return setting.value;
    });
    try {
        drawn.scenario = detail::readScenario(document, impl_->source);
    } catch (const ScenarioError& error) {
        throw ScenarioError(std::format("{}, drawn with settings seed {}: {}", impl_->source,
                                        settingsSeed, error.what()));
    }
    return drawn;
}

std::string EpisodeScenarios::text(std::uint64_t settingsSeed) const {
    if (impl_->ranges().empty()) {
        return impl_->text;
    }
    const toml::table document = impl_->with([&](const SettingRange& range) {
        return drawSetting(range, settingsSeed).value;
    });
    std::ostringstream out;
    out << document << '\n';
    return out.str();
}

const Scenario& EpisodeScenarios::base() const noexcept { return impl_->base; }

std::vector<std::string> EpisodeScenarios::settingNames() const {
    std::vector<std::string> names;
    for (const SettingRange& range : impl_->ranges()) {
        names.push_back(range.path());
    }
    return names;
}

// ---------------------------------------------------------------------------------------------
// One market

struct LearningMarket::Episode {
    // Passes everything on to the caller's sink, and stops the run when the scorer stops the seat.
    class Watch final : public EventSink {
    public:
        explicit Watch(Episode& episode) noexcept : episode_(episode) {}
        EventSink* next = nullptr;

        void onRequest(Timestamp time, AgentId agent, const Request& request) override {
            if (next != nullptr) {
                next->onRequest(time, agent, request);
            }
        }
        void onEvent(Timestamp time, const Event& event) override {
            if (next != nullptr) {
                next->onEvent(time, event);
            }
            // The scorer, ahead of this sink, has seen the trade and its fills.
            if (std::holds_alternative<Trade>(event) && !episode_.stopped &&
                episode_.scorer != nullptr && episode_.scorer->stoppedAt(episode_.seat)) {
                episode_.stopped = true;
                episode_.interrupt();
            }
        }

    private:
        Episode& episode_;
    };

    Episode(const LearningConfig& shape, const LearningConfig& episodeConfig)
        : config(episodeConfig), watch(*this), rows(shape.maxEvents * kEventColumns),
          feedRows(shape.maxEvents * kEventColumns),
          depthRing(shape.maxDepthUpdates * 4 * shape.depthLevels) {}

    LearningConfig config;
    Scenario scenario;
    std::optional<std::string> text;
    Watch watch;
    std::optional<SessionMarket> market;
    Simulation* simulation = nullptr;
    Scorer* scorer = nullptr;
    AgentId seat = 0;
    Mark rewardMark = Mark::Book;
    Mark closeMark = Mark::Book;
    // Copies of the true value, made from the same seed, so that reading them changes nothing.
    std::shared_ptr<Fundamental> value;
    std::shared_ptr<Fundamental> feedValue;
    Random feedNoise{0, 0};
    std::int64_t nextFeed = 0; // the next publication, counted in feed intervals from the start
    // The latest publication the seat has heard, and the one it had heard at its last decision,
    // in the underlying's units; NaN before the first.
    double feedHeard = std::numeric_limits<double>::quiet_NaN();
    double feedAtDecision = std::numeric_limits<double>::quiet_NaN();
    bool decisionDue = false; // the run was stopped for a decision: a fill, or the loss limit
    Timestamp end = 0;
    Timestamp lastDecision = 0;
    Points lastScore = 0;
    bool stopped = false;
    bool finished = false;

    // The market as the seat has heard it.
    std::optional<LevelSummary> bid;
    std::optional<LevelSummary> ask;
    std::optional<Price> lastTrade;
    Phase phase = Phase::Continuous;
    BookDepth book;
    std::int64_t rejections = 0;
    // The seat's resting order on each side, bid then offer; 0 for none.
    std::array<ClientOrderId, 2> quotes{};
    // The side of each of the seat's live orders, for the events that do not say.
    std::vector<std::pair<ClientOrderId, Side>> sides;

    // What the seat heard since the last decision: rings that keep the latest rows.
    std::vector<std::int64_t> rows;
    std::uint64_t rowCount = 0;
    std::vector<std::int64_t> feedRows;
    std::uint64_t feedCount = 0;
    std::vector<std::int64_t> depthRing;
    std::uint64_t depthCount = 0;

    std::vector<SessionAction> actions; // every request the seat sent, for session()

    // How much of each output was written last time, so only what is stale is cleared.
    struct Written {
        const std::int64_t* data = nullptr;
        std::size_t size = 0;
    };
    std::array<Written, 4> written{};

    [[nodiscard]] Timestamp now() const noexcept { return simulation->now(); }

    void push(std::vector<std::int64_t>& ring, std::uint64_t& count, LearningEvent type,
              Timestamp time, std::int64_t a = 0, std::int64_t b = 0, std::int64_t c = 0,
              std::int64_t d = 0, std::int64_t e = 0) {
        const std::size_t capacity = ring.size() / kEventColumns;
        std::int64_t* row = ring.data() + (count % capacity) * kEventColumns;
        row[0] = static_cast<std::int64_t>(type);
        row[1] = time;
        row[2] = a;
        row[3] = b;
        row[4] = c;
        row[5] = d;
        row[6] = e;
        ++count;
    }

    [[nodiscard]] std::int64_t sideOf(ClientOrderId id) const noexcept {
        for (const auto& [known, side] : sides) {
            if (known == id) {
                return sign(side);
            }
        }
        return 0;
    }

    void forget(ClientOrderId id) {
        std::erase_if(sides, [id](const auto& entry) { return entry.first == id; });
    }

    void writeLevels(std::int64_t* out, const BookDepth& depth, std::size_t levels) const {
        std::fill_n(out, 4 * levels, 0);
        for (const auto& [side, from] : {std::pair{0, &depth.bids}, std::pair{1, &depth.asks}}) {
            const std::size_t count = std::min(levels, from->size());
            for (std::size_t i = 0; i < count; ++i) {
                out[(static_cast<std::size_t>(side) * levels + i) * 2] = (*from)[i].price;
                out[(static_cast<std::size_t>(side) * levels + i) * 2 + 1] = (*from)[i].quantity;
            }
        }
    }

    // An event the seat heard at `time`.
    void hear(Timestamp time, const Event& event) {
        std::visit(
            detail::Overloaded{
                [&](const Trade& trade) {
                    lastTrade = trade.price;
                    push(rows, rowCount, LearningEvent::Trade, time, trade.price, trade.quantity,
                         sign(trade.aggressorSide), trade.auction ? 1 : 0);
                },
                [&](const TopOfBook& top) {
                    bid = top.bid;
                    ask = top.ask;
                    push(rows, rowCount, LearningEvent::Quote, time, bid ? bid->price : 0,
                         bid ? bid->quantity : 0, ask ? ask->price : 0, ask ? ask->quantity : 0);
                },
                [&](const BookDepth& depth) {
                    book = depth;
                    const std::size_t capacity = config.maxDepthUpdates;
                    const std::size_t levels = config.depthLevels;
                    if (capacity > 0) {
                        writeLevels(depthRing.data() + (depthCount % capacity) * 4 * levels, depth,
                                    levels);
                    }
                    push(rows, rowCount, LearningEvent::Depth, time,
                         static_cast<std::int64_t>(depthCount));
                    ++depthCount;
                },
                [&](const PhaseChanged& changed) {
                    phase = changed.phase;
                    push(rows, rowCount, LearningEvent::Phase, time,
                         static_cast<std::int64_t>(changed.phase), changed.price.value_or(0));
                },
                [&](const Indicative& indicative) {
                    const auto& uncross = indicative.uncross;
                    push(rows, rowCount, LearningEvent::Indicative, time,
                         uncross ? uncross->price : 0, uncross ? uncross->volume : 0,
                         uncross ? uncross->imbalance : 0);
                },
                [&](const OrderAccepted& accepted) {
                    push(rows, rowCount, LearningEvent::Accepted, time, sign(accepted.side),
                         accepted.price, accepted.quantity,
                         accepted.type == OrderType::Market ? 1 : 0);
                },
                [&](const OrderModified& modified) {
                    push(rows, rowCount, LearningEvent::Modified, time,
                         sideOf(modified.clientOrderId), modified.price, modified.quantity);
                },
                [&](const OrderFilled& filled) {
                    push(rows, rowCount, LearningEvent::Fill, time, sign(filled.side),
                         filled.price, filled.quantity, static_cast<std::int64_t>(filled.liquidity),
                         filled.fee);
                    if (filled.leavesQuantity == 0) {
                        forget(filled.clientOrderId);
                    }
                    // A fill is a decision of its own, once everything due now has run.
                    interrupt();
                },
                [&](const OrderCancelled& cancelled) {
                    push(rows, rowCount, LearningEvent::Cancelled, time,
                         sideOf(cancelled.clientOrderId), cancelled.quantity,
                         static_cast<std::int64_t>(cancelled.reason));
                    forget(cancelled.clientOrderId);
                },
                [&](const OrderRejected& rejected) {
                    ++rejections;
                    push(rows, rowCount, LearningEvent::Rejected, time,
                         sideOf(rejected.clientOrderId),
                         static_cast<std::int64_t>(rejected.request),
                         static_cast<std::int64_t>(rejected.reason));
                    if (rejected.request == RequestKind::New) {
                        forget(rejected.clientOrderId);
                    }
                },
            },
            event);
    }

    // Stops the run for a decision once everything due now has run.
    void interrupt() noexcept {
        decisionDue = true;
        simulation->interrupt();
    }

    // The feed's publications up to now: the underlying `lag` before each, with fresh noise.
    // Returns whether one of them moved the feed's trigger from what the last decision heard.
    bool publishFeed() {
        if (!config.feed || !feedValue) {
            return false;
        }
        const LearningFeed& feed = *config.feed;
        const Timestamp now = this->now();
        bool moved = false;
        while (nextFeed <= now / feed.interval) {
            const Timestamp time = nextFeed * feed.interval;
            const double seen = feedValue->valueAt(std::max<Timestamp>(time - feed.lag, 0));
            double underlying = feed.value ? seen : feedValue->walk();
            if (feed.noise > 0.0) {
                underlying += feedNoise.normal(0.0, feed.noise);
            }
            push(feedRows, feedCount, LearningEvent::Feed, time,
                 static_cast<std::int64_t>(std::llround(underlying * 1e6)));
            ++nextFeed;
            feedHeard = underlying;
            moved = moved || (feed.trigger > 0.0 && !std::isnan(feedAtDecision) &&
                              std::abs(underlying - feedAtDecision) >= feed.trigger);
        }
        return moved;
    }

    // Runs to `until`, or until the run is stopped for a decision sooner: by a fill or the loss
    // limit, or, with a trigger on the feed, at the first publication that moves it. With a
    // trigger the market runs one publication at a time, which processes the same items in the
    // same order as running straight to the end.
    void runToDecision(Timestamp until) {
        decisionDue = false;
        if (!config.feed || config.feed->trigger <= 0.0 || !feedValue) {
            simulation->runUntil(until);
            return;
        }
        const Duration interval = config.feed->interval;
        while (now() < until) {
            simulation->runUntil(std::min(nextFeed * interval, until));
            if (decisionDue || publishFeed()) {
                return;
            }
        }
    }

    // Clears what was written beyond `size` last time into the same buffer.
    void clearStale(std::size_t slot, std::span<std::int64_t> out, std::size_t size) {
        Written& last = written[slot];
        const std::size_t from = std::min(size, out.size());
        const std::size_t to =
            last.data == out.data() ? std::min(last.size, out.size()) : out.size();
        if (to > from) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(from),
                      out.begin() + static_cast<std::ptrdiff_t>(to), 0);
        }
        last = {.data = out.data(), .size = size};
    }

    void observe(const LearningObservation& out) {
        publishFeed();
        feedAtDecision = feedHeard;
        const std::size_t capacity = config.maxEvents;
        const std::size_t levels = config.depthLevels;

        // The latest rows of the market's events and of the feed, merged in time order; at the
        // same time the market's come first.
        const std::size_t marketKept = std::min<std::uint64_t>(rowCount, capacity);
        const std::size_t feedKept = std::min<std::uint64_t>(feedCount, capacity);
        const std::size_t kept = std::min(capacity, marketKept + feedKept);
        const auto rowAt = [&](const std::vector<std::int64_t>& ring, std::uint64_t count,
                               std::size_t keptOf, std::size_t i) {
            return ring.data() + ((count - keptOf + i) % capacity) * kEventColumns;
        };
        const std::uint64_t depthKept = std::min<std::uint64_t>(depthCount, config.maxDepthUpdates);
        const std::uint64_t firstDepth = depthCount - depthKept;
        std::size_t m = marketKept;
        std::size_t f = feedKept;
        for (std::size_t k = kept; k-- > 0;) {
            const std::int64_t* row = nullptr;
            if (f > 0 && (m == 0 || rowAt(feedRows, feedCount, feedKept, f - 1)[1] >=
                                        rowAt(rows, rowCount, marketKept, m - 1)[1])) {
                row = rowAt(feedRows, feedCount, feedKept, --f);
            } else {
                row = rowAt(rows, rowCount, marketKept, --m);
            }
            std::int64_t* to = out.events.data() + k * kEventColumns;
            std::copy_n(row, kEventColumns, to);
            if (to[0] == static_cast<std::int64_t>(LearningEvent::Depth)) {
                const auto sequence = static_cast<std::uint64_t>(to[2]);
                to[2] = sequence >= firstDepth ? static_cast<std::int64_t>(sequence - firstDepth)
                                               : -1;
            }
        }
        clearStale(0, out.events, kept * kEventColumns);

        const std::size_t depthSize = 4 * levels;
        for (std::uint64_t i = 0; i < depthKept; ++i) {
            const std::uint64_t slot = (firstDepth + i) % config.maxDepthUpdates;
            std::copy_n(depthRing.data() + slot * depthSize, depthSize,
                        out.depth.data() + i * depthSize);
        }
        clearStale(1, out.depth, static_cast<std::size_t>(depthKept) * depthSize);
        writeLevels(out.book.data(), book, levels);

        const Ledger& ledger = simulation->ledger(seat);
        const AccountConfig& account = scenario.participant.account;
        const Timestamp now = this->now();
        const std::array<std::int64_t, kLearningStateNames.size()> state{
            now,
            end - now,
            ledger.position(),
            ledger.cash() - account.initialCash,
            ledger.fees(),
            ledger.openQuantity(Side::Buy),
            ledger.openQuantity(Side::Sell),
            bid ? bid->price : 0,
            bid ? bid->quantity : 0,
            ask ? ask->price : 0,
            ask ? ask->quantity : 0,
            lastTrade.value_or(0),
            static_cast<std::int64_t>(phase),
            scenario.referencePrice,
            simulation->exchange().config().maxPrice,
            account.maxPosition,
            account.maxOrderQuantity,
            rejections,
        };
        std::ranges::copy(state, out.state.begin());

        const auto orders = ledger.orders();
        const std::size_t listed = std::min(orders.size(), config.maxOrders);
        for (std::size_t i = 0; i < listed; ++i) {
            const OwnOrder& order = orders[i].second;
            std::int64_t* to = out.orders.data() + i * kOrderColumns;
            to[0] = sign(order.side);
            to[1] = order.price;
            to[2] = order.leaves;
            to[3] = order.acknowledged ? 1 : 0;
            to[4] = order.cancelRequested ? 1 : 0;
        }
        clearStale(2, out.orders, listed * kOrderColumns);

        out.counts[0] = static_cast<std::int64_t>(rowCount + feedCount);
        out.counts[1] = static_cast<std::int64_t>(depthCount);
        out.counts[2] = static_cast<std::int64_t>(orders.size());

        rowCount = 0;
        feedCount = 0;
        depthCount = 0;
        rejections = 0;
    }

    [[nodiscard]] Points scoreAt(Mark mark) const {
        std::optional<double> at;
        if (mark == Mark::Value) {
            at = value->valueAt(now());
        }
        return scorer->score(seat, now(), at, mark).total;
    }

    // The seat's live resting order on a side, if it has one.
    [[nodiscard]] const OwnOrder* quote(const Ledger& ledger, Side side) const {
        const ClientOrderId id = quotes[index(side)];
        const OwnOrder* order = id != 0 ? ledger.find(id) : nullptr;
        return order != nullptr && !order->cancelRequested ? order : nullptr;
    }

    // The best price on a side as the seat has heard it, its own resting order there left out,
    // a cancel of it on its way or not: the next level of the depth it heard when its order is
    // alone at the best price, or nothing when it has no depth to say.
    [[nodiscard]] std::optional<Price> othersBest(const Ledger& ledger, Side side) const {
        const std::optional<LevelSummary>& best = side == Side::Buy ? bid : ask;
        if (!best) {
            return std::nullopt;
        }
        const ClientOrderId id = quotes[index(side)];
        const OwnOrder* own = id != 0 ? ledger.find(id) : nullptr;
        if (own == nullptr || !own->acknowledged || own->price != best->price ||
            best->quantity > own->leaves) {
            return best->price;
        }
        for (const LevelSummary& level : side == Side::Buy ? book.bids : book.asks) {
            if (level.price != own->price) {
                return level.price;
            }
        }
        return std::nullopt;
    }

    // Applies a decision through the seat's context, recording every request it sends.
    class Decision {
    public:
        Decision(Episode& episode, AgentContext& context) noexcept
            : episode_(episode), context_(context), ledger_(context.ledger()),
              account_(episode.scenario.participant.account) {}

        void apply(const LearningOrders& orders) {
            Side taken = Side::Buy;
            if (orders.take != 0) {
                taken = orders.take > 0 ? Side::Buy : Side::Sell;
                take(taken, orders.take > 0 ? orders.take : -orders.take, orders.takePrice);
            }
            for (const auto& [side, price, size] :
                 {std::tuple{Side::Buy, orders.bidPrice, orders.bidSize},
                  std::tuple{Side::Sell, orders.askPrice, orders.askSize}}) {
                if (orders.take != 0 && side == opposite(taken)) {
                    continue; // cancelled to take from; not placed again until the next decision
                }
                if (size < 1 || price < 1) {
                    cancelQuote(side);
                } else {
                    quoteAtPrice(side, price, size);
                }
            }
        }

        void apply(LearningAction action, Quantity size) {
            switch (action) {
            case LearningAction::Hold:
                return;
            case LearningAction::BidJoin:
            case LearningAction::BidImprove:
            case LearningAction::BidBehind:
                quoteAt(Side::Buy, action, size);
                return;
            case LearningAction::AskJoin:
            case LearningAction::AskImprove:
            case LearningAction::AskBehind:
                quoteAt(Side::Sell, action, size);
                return;
            case LearningAction::BothJoin:
                quoteAt(Side::Buy, LearningAction::BidJoin, size);
                quoteAt(Side::Sell, LearningAction::AskJoin, size);
                return;
            case LearningAction::BothImprove: {
                // With two ticks between them, improving both would put them on one price,
                // where the second would meet the first: both join instead.
                const std::optional<Price> bestBid = episode_.othersBest(ledger_, Side::Buy);
                const std::optional<Price> bestAsk = episode_.othersBest(ledger_, Side::Sell);
                const bool meet = bestBid && bestAsk && *bestBid + 1 >= *bestAsk - 1;
                quoteAt(Side::Buy, meet ? LearningAction::BidJoin : LearningAction::BidImprove,
                        size);
                quoteAt(Side::Sell, meet ? LearningAction::AskJoin : LearningAction::AskImprove,
                        size);
                return;
            }
            case LearningAction::BothBehind:
                quoteAt(Side::Buy, LearningAction::BidBehind, size);
                quoteAt(Side::Sell, LearningAction::AskBehind, size);
                return;
            case LearningAction::CancelBid:
                cancelQuote(Side::Buy);
                return;
            case LearningAction::CancelAsk:
                cancelQuote(Side::Sell);
                return;
            case LearningAction::CancelAll:
                cancelQuote(Side::Buy);
                cancelQuote(Side::Sell);
                return;
            case LearningAction::Buy:
                take(Side::Buy, size);
                return;
            case LearningAction::Sell:
                take(Side::Sell, size);
                return;
            case LearningAction::Close:
                close();
                return;
            }
        }

    private:
        // Lots the seat may still add on a side without the position limit's check failing,
        // leaving out the open quantity of `ignored`, an order about to be replaced.
        [[nodiscard]] Quantity room(Side side, Quantity ignored) const {
            const Quantity open = ledger_.openQuantity(side) - ignored;
            return side == Side::Buy ? account_.maxPosition - ledger_.position() - open
                                     : account_.maxPosition + ledger_.position() - open;
        }

        [[nodiscard]] Quantity sized(Quantity wanted, Quantity available) const {
            return std::min({wanted, available, account_.maxOrderQuantity});
        }

        ClientOrderId submit(Side side, Price price, Quantity quantity, TimeInForce timeInForce) {
            const ClientOrderId id = context_.submitLimit(side, price, quantity, timeInForce);
            episode_.sides.emplace_back(id, side);
            episode_.actions.push_back(
                {.time = context_.now(),
                 .request = NewOrder{.clientOrderId = id,
                                     .side = side,
                                     .type = OrderType::Limit,
                                     .timeInForce = timeInForce,
                                     .price = price,
                                     .quantity = quantity}});
            return id;
        }

        void cancel(ClientOrderId id) {
            context_.cancel(id);
            episode_.actions.push_back(
                {.time = context_.now(), .request = CancelOrder{.clientOrderId = id}});
        }

        void modify(ClientOrderId id, Price price, Quantity quantity) {
            context_.modify(id, price, quantity);
            episode_.actions.push_back(
                {.time = context_.now(),
                 .request =
                     ModifyOrder{.clientOrderId = id, .price = price, .quantity = quantity}});
        }

        void quoteAt(Side side, LearningAction how, Quantity size) {
            const std::optional<Price> best = episode_.othersBest(ledger_, side);
            if (!best) {
                return;
            }
            const Price better = side == Side::Buy ? 1 : -1;
            Price price = *best;
            if (how == LearningAction::BidImprove || how == LearningAction::AskImprove) {
                price = *best + better;
                // Never at or through the other side, the seat's own order there included, where
                // a post-only order would be turned away.
                const std::optional<LevelSummary>& facing =
                    side == Side::Buy ? episode_.ask : episode_.bid;
                if (facing && (side == Side::Buy ? price >= facing->price
                                                 : price <= facing->price)) {
                    price = *best;
                }
            } else if (how == LearningAction::BidBehind || how == LearningAction::AskBehind) {
                price = *best - better;
            }
            quoteAtPrice(side, price, size);
        }

        // Keeps the seat's quote on `side` at `price`, moving the one it has there, or leaving it
        // alone where it is already at that price.
        void quoteAtPrice(Side side, Price price, Quantity size) {
            if (price < 1 || price > context_.maxPrice()) {
                return;
            }
            const OwnOrder* own = episode_.quote(ledger_, side);
            if (own != nullptr) {
                if (own->price != price) {
                    // Moving loses the queue place anyway; resize to what the limit allows, or
                    // keep the size when it allows none more.
                    const Quantity quantity = sized(size, room(side, own->leaves));
                    modify(own->clientOrderId, price, quantity >= 1 ? quantity : own->leaves);
                }
                return;
            }
            const Quantity quantity = sized(size, room(side, 0));
            if (quantity >= 1) {
                episode_.quotes[index(side)] =
                    submit(side, price, quantity, TimeInForce::PostOnly);
            }
        }

        void cancelQuote(Side side) {
            if (const OwnOrder* own = episode_.quote(ledger_, side)) {
                cancel(own->clientOrderId);
            }
        }

        // An immediate-or-cancel order on `side` at `limit`, or with none at the best price on the
        // other, take_ticks past.
        void takeAt(Side side, Quantity quantity, Price limit = 0) {
            if (limit > 0) {
                if (quantity >= 1 && limit <= context_.maxPrice()) {
                    submit(side, limit, quantity, TimeInForce::ImmediateOrCancel);
                }
                return;
            }
            const std::optional<Price> facing = episode_.othersBest(ledger_, opposite(side));
            if (!facing || quantity < 1) {
                return;
            }
            const Price ticks = episode_.config.takeTicks;
            const Price price = side == Side::Buy ? std::min(*facing + ticks, context_.maxPrice())
                                                  : std::max<Price>(*facing - ticks, 1);
            submit(side, price, quantity, TimeInForce::ImmediateOrCancel);
        }

        // Cancels the seat's own order on the side it takes from first, which the book would
        // otherwise meet before anyone else's and turn the order away as a trade with itself.
        void take(Side side, Quantity size, Price limit = 0) {
            cancelQuote(opposite(side));
            takeAt(side, sized(size, room(side, 0)), limit);
        }

        void close() {
            cancelQuote(Side::Buy);
            cancelQuote(Side::Sell);
            const Quantity position = ledger_.position();
            if (position == 0) {
                return;
            }
            const Side side = position > 0 ? Side::Sell : Side::Buy;
            // The cancels reach the exchange first, so the orders they cancel take no room.
            Quantity cancelled = 0;
            for (const auto& [id, order] : ledger_.orders()) {
                if (order.side == side && order.cancelRequested) {
                    cancelled += order.leaves;
                }
            }
            takeAt(side, sized(position > 0 ? position : -position, room(side, cancelled)));
        }

        Episode& episode_;
        AgentContext& context_;
        const Ledger& ledger_;
        const AccountConfig& account_;
    };
};

LearningMarket::LearningMarket(LearningConfig shape, const AgentRegistry& registry, EventSink* sink)
    : shape_(std::move(shape)), registry_(&registry), sink_(sink) {
    if (shape_.actions.empty() || shape_.sizes.empty() || shape_.maxEvents == 0 ||
        shape_.maxOrders == 0) {
        throw std::invalid_argument(
            "a learning market needs actions, sizes, and room for events and orders");
    }
}

LearningMarket::LearningMarket(LearningMarket&&) noexcept = default;
LearningMarket& LearningMarket::operator=(LearningMarket&&) noexcept = default;
LearningMarket::~LearningMarket() = default;

namespace {

// Throws std::invalid_argument unless `out` has the sizes the shape gives every buffer.
void checkSizes(const LearningConfig& shape, const LearningObservation& out) {
    const std::size_t levels = 2 * shape.depthLevels * 2;
    if (out.state.size() != kLearningStateNames.size() ||
        out.orders.size() != shape.maxOrders * kOrderColumns ||
        out.events.size() != shape.maxEvents * kEventColumns ||
        out.depth.size() != shape.maxDepthUpdates * levels || out.book.size() != levels ||
        out.counts.size() != kCountColumns) {
        throw std::invalid_argument("the observation's buffers are not the sizes the learning "
                                    "market's shape gives them");
    }
}

} // namespace

LearningStep LearningMarket::reset(Scenario scenario, const LearningObservation& out,
                                   std::optional<std::string> text) {
    // Everything is checked before the episode there is gives way to the new one.
    checkSizes(shape_, out);
    const LearningConfig config = scenario.learning.value_or(shape_);
    if (!sameShape(config, shape_)) {
        throw ScenarioError("the scenario's [learning] section gives other actions, sizes or "
                            "capacity than the market's");
    }
    if (config.warmup >= scenario.duration) {
        throw ScenarioError("the first decision, at 'warmup', must come before the end of the run");
    }
    if (config.interval <= 0 || (config.feed && config.feed->interval < kMinFeedInterval)) {
        throw ScenarioError("decisions need a positive interval, and a feed one of at least 1ms");
    }
    if (config.feed && !hasValue(scenario)) {
        throw ScenarioError("a feed of the underlying needs a fundamental or a prediction market");
    }
    auto episode = std::make_unique<Episode>(shape_, config);
    episode->rewardMark = config.rewardMark.value_or(hasValue(scenario) ? Mark::Value : Mark::Book);
    episode->closeMark = config.close.value_or(scenario.prediction ? Mark::Value : Mark::Book);
    if ((episode->rewardMark == Mark::Value || episode->closeMark == Mark::Value) &&
        !hasValue(scenario)) {
        throw ScenarioError("valuing at the true value needs a fundamental or a prediction market");
    }
    if (!scenario.scoring) {
        scenario.scoring = ScoringConfig{};
    }
    episode->end = sessionEnd(scenario);
    episode->watch.next = sink_;
    episode->market.emplace(
        openSession(scenario, *registry_, &episode->watch, {std::string{kLearningSeat}}));
    Episode& e = *episode;
    const Seat& seat = e.market->seats.front();
    e.seat = seat.agent;
    e.simulation = &e.market->run.simulation();
    e.scorer = e.market->scorer.get();
    seat.participant->setListener(
        [&e](AgentContext& context, const Event& event) { e.hear(context.now(), event); });
    if (hasValue(scenario)) {
        e.value = makeFundamental(scenario);
        if (config.feed) {
            // The same value from the same start: a copy, rather than its search for where to start
            // made again.
            e.feedValue = std::make_shared<Fundamental>(*e.value);
            e.feedNoise = Random{scenario.seed, kFeedStream};
        }
    }
    e.scenario = std::move(scenario);
    e.text = std::move(text);
    episode_ = std::move(episode);

    while (e.now() < config.warmup) {
        e.simulation->runUntil(config.warmup);
    }
    e.lastDecision = e.now();
    // The first reward counts from the start, so that the rewards add up to the score even when
    // the seat starts with one, as with a target or a position held from the start.
    e.lastScore = 0;
    LearningStep report{.elapsed = e.now(),
                        .score = e.scoreAt(e.closeMark),
                        .value = e.value ? e.value->valueAt(e.now())
                                         : std::numeric_limits<double>::quiet_NaN()};
    e.observe(out);
    return report;
}

LearningMarket::Episode& LearningMarket::stepping(const LearningObservation& out) {
    if (!episode_) {
        throw std::logic_error("the market has no episode: reset it first");
    }
    Episode& e = *episode_;
    if (e.finished) {
        throw std::logic_error("the episode is over: reset the market for another");
    }
    checkSizes(shape_, out);
    return e;
}

LearningStep LearningMarket::step(std::size_t action, std::size_t size,
                                  const LearningObservation& out) {
    Episode& e = stepping(out);
    if (action >= shape_.actions.size() || size >= shape_.sizes.size()) {
        throw std::invalid_argument(std::format(
            "action {} at size {}: there are {} actions and {} sizes", action, size,
            shape_.actions.size(), shape_.sizes.size()));
    }
    const LearningAction chosen = shape_.actions[action];
    const Quantity quantity = shape_.sizes[size];
    e.simulation->act(e.seat, [&](AgentContext& context) {
        Episode::Decision{e, context}.apply(chosen, quantity);
    });
    return advance(out);
}

LearningStep LearningMarket::stepOrders(const LearningOrders& orders,
                                        const LearningObservation& out) {
    Episode& e = stepping(out);
    if (orders.bidPrice < 0 || orders.askPrice < 0 || orders.bidSize < 0 || orders.askSize < 0 ||
        orders.takePrice < 0) {
        throw std::invalid_argument("orders' prices and sizes must not be negative");
    }
    e.simulation->act(e.seat,
                      [&](AgentContext& context) { Episode::Decision{e, context}.apply(orders); });
    return advance(out);
}

LearningStep LearningMarket::advance(const LearningObservation& out) {
    Episode& e = *episode_;
    const Duration interval = e.config.interval;
    const Timestamp next = (e.now() / interval + 1) * interval;
    e.runToDecision(std::min(next, e.end));

    LearningStep report{.elapsed = e.now() - e.lastDecision};
    e.lastDecision = e.now();
    e.finished = e.now() >= e.end || e.stopped;
    if (e.finished) {
        report.score = e.scoreAt(e.closeMark);
        report.reward = static_cast<double>(report.score - e.lastScore) /
                        static_cast<double>(kPointsPerTickLot);
        e.lastScore = report.score;
    } else {
        const Points score = e.scoreAt(e.rewardMark);
        report.reward =
            static_cast<double>(score - e.lastScore) / static_cast<double>(kPointsPerTickLot);
        e.lastScore = score;
        report.score = e.closeMark == e.rewardMark ? score : e.scoreAt(e.closeMark);
    }
    report.terminated = e.finished;
    report.stopped = e.stopped;
    report.value = e.value ? e.value->valueAt(e.now()) : std::numeric_limits<double>::quiet_NaN();
    e.observe(out);
    return report;
}

bool LearningMarket::started() const noexcept { return episode_ != nullptr; }

bool LearningMarket::finished() const noexcept { return episode_ && episode_->finished; }

Timestamp LearningMarket::now() const {
    if (!episode_) {
        throw std::logic_error("the market has no episode: reset it first");
    }
    return episode_->now();
}

Session LearningMarket::session() const {
    if (!episode_ || !episode_->text) {
        throw std::logic_error("only an episode started from a scenario's text can be a session");
    }
    return Session{.scenario = *episode_->text,
                   .seed = episode_->scenario.seed,
                   .end = episode_->now(),
                   .seats = {std::string{kLearningSeat}},
                   .actions = episode_->actions};
}

const LearningConfig& LearningMarket::shape() const noexcept { return shape_; }

// ---------------------------------------------------------------------------------------------
// The pool

struct LearningPool::State {
    State(std::string text, std::string source, std::size_t count, std::size_t threadCount,
          const AgentRegistry& agents)
        : registry(agents),
          scenarios(std::move(text), std::move(source), registry),
          config(scenarios.base().learning.value_or(LearningConfig{})), markets(count),
          names(scenarios.settingNames()), workers(threadCount) {}

    AgentRegistry registry;
    EpisodeScenarios scenarios;
    LearningConfig config;
    std::size_t markets;
    std::vector<std::string> names;
    std::vector<LearningMarket> each;

    std::vector<std::int64_t> stateBuffer, orderBuffer, eventBuffer, depthBuffer, bookBuffer,
        countBuffer, elapsedBuffer, scoreBuffer;
    std::vector<double> rewardBuffer, valueBuffer, settingBuffer;
    std::vector<std::uint8_t> terminatedBuffer, truncatedBuffer, stoppedBuffer;
    std::vector<std::uint64_t> seedBuffer, settingsSeedBuffer;
    std::vector<std::exception_ptr> errors;
    Workers workers;
    // Held by reset, step and session, so that calls from several threads take turns.
    std::mutex calls;

    template <typename T>
    static std::span<T> slice(std::vector<T>& buffer, std::size_t market, std::size_t size) {
        return std::span<T>{buffer}.subspan(market * size, size);
    }

    [[nodiscard]] LearningObservation observation(std::size_t market) {
        const std::size_t levels = 4 * config.depthLevels;
        return {.state = slice(stateBuffer, market, kLearningStateNames.size()),
                .orders = slice(orderBuffer, market, config.maxOrders * kOrderColumns),
                .events = slice(eventBuffer, market, config.maxEvents * kEventColumns),
                .depth = slice(depthBuffer, market, config.maxDepthUpdates * levels),
                .book = slice(bookBuffer, market, levels),
                .counts = slice(countBuffer, market, kCountColumns)};
    }

    void report(std::size_t market, const LearningStep& step) {
        rewardBuffer[market] = step.reward;
        terminatedBuffer[market] = step.terminated ? 1 : 0;
        truncatedBuffer[market] = 0;
        stoppedBuffer[market] = step.stopped ? 1 : 0;
        elapsedBuffer[market] = step.elapsed;
        scoreBuffer[market] = step.score;
        valueBuffer[market] = step.value;
    }

    // Runs `task` on every market and rethrows the first market's exception, if any.
    void forEach(const std::function<void(std::size_t)>& task) {
        std::ranges::fill(errors, nullptr);
        workers.run(markets, [&](std::size_t market) {
            try {
                task(market);
            } catch (...) {
                errors[market] = std::current_exception();
            }
        });
        for (const std::exception_ptr& error : errors) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
    }
};

LearningPool::LearningPool(std::string text, std::string source, std::size_t markets,
                           std::size_t threads, const AgentRegistry& registry) {
    if (markets == 0) {
        throw std::invalid_argument("a pool needs at least one market");
    }
    if (threads == 0) {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    threads = std::min(threads, markets);
    state_ =
        std::make_unique<State>(std::move(text), std::move(source), markets, threads, registry);
    State& s = *state_;
    s.each.reserve(markets);
    for (std::size_t i = 0; i < markets; ++i) {
        s.each.emplace_back(s.config, s.registry);
    }
    const LearningConfig& c = s.config;
    s.stateBuffer.assign(markets * kLearningStateNames.size(), 0);
    s.orderBuffer.assign(markets * c.maxOrders * kOrderColumns, 0);
    s.eventBuffer.assign(markets * c.maxEvents * kEventColumns, 0);
    s.depthBuffer.assign(markets * c.maxDepthUpdates * 4 * c.depthLevels, 0);
    s.bookBuffer.assign(markets * 4 * c.depthLevels, 0);
    s.countBuffer.assign(markets * kCountColumns, 0);
    s.elapsedBuffer.assign(markets, 0);
    s.scoreBuffer.assign(markets, 0);
    s.rewardBuffer.assign(markets, 0.0);
    s.valueBuffer.assign(markets, std::numeric_limits<double>::quiet_NaN());
    s.settingBuffer.assign(markets * s.names.size(), std::numeric_limits<double>::quiet_NaN());
    s.terminatedBuffer.assign(markets, 0);
    s.truncatedBuffer.assign(markets, 0);
    s.stoppedBuffer.assign(markets, 0);
    s.seedBuffer.assign(markets, 0);
    s.settingsSeedBuffer.assign(markets, 0);
    s.errors.assign(markets, nullptr);
}

LearningPool::~LearningPool() = default;

void LearningPool::reset(std::span<const std::uint64_t> seeds,
                         std::span<const std::uint64_t> settingsSeeds,
                         std::span<const std::uint8_t> mask,
                         std::span<const std::optional<std::string>> scenarios) {
    State& s = *state_;
    const std::scoped_lock lock{s.calls};
    const auto check = [&](std::size_t size, std::string_view what, bool required) {
        if ((required || size != 0) && size != s.markets) {
            throw std::invalid_argument(
                std::format("{} must have one entry a market: {} for {} markets", what, size,
                            s.markets));
        }
    };
    check(seeds.size(), "the seeds", true);
    check(settingsSeeds.size(), "the settings seeds", false);
    check(mask.size(), "the mask", false);
    check(scenarios.size(), "the scenarios", false);
    s.forEach([&](std::size_t market) {
        if (!mask.empty() && mask[market] == 0) {
            return;
        }
        const std::uint64_t settingsSeed =
            settingsSeeds.empty() ? seeds[market] : settingsSeeds[market];
        const bool own = !scenarios.empty() && scenarios[market].has_value();
        std::optional<EpisodeScenarios> replacement;
        if (own) {
            replacement.emplace(*scenarios[market],
                                std::format("the scenario for market {}", market), s.registry,
                                false);
        }
        const EpisodeScenarios& from = own ? *replacement : s.scenarios;
        EpisodeScenarios::Drawn drawn = from.draw(settingsSeed);
        drawn.scenario.seed = seeds[market];
        const LearningStep first = s.each[market].reset(std::move(drawn.scenario),
                                                        s.observation(market),
                                                        from.text(settingsSeed));
        s.report(market, first);
        const auto settings = State::slice(s.settingBuffer, market, s.names.size());
        if (!own) {
            std::ranges::copy(drawn.settings, settings.begin());
        } else {
            std::ranges::fill(settings, std::numeric_limits<double>::quiet_NaN());
        }
        s.seedBuffer[market] = seeds[market];
        s.settingsSeedBuffer[market] = settingsSeed;
    });
}

void LearningPool::step(std::span<const std::int64_t> actions) {
    State& s = *state_;
    const std::scoped_lock lock{s.calls};
    if (actions.size() != 2 * s.markets) {
        throw std::invalid_argument(std::format(
            "the actions must be two numbers a market, the action and the size: {} for {} "
            "markets",
            actions.size(), s.markets));
    }
    for (std::size_t market = 0; market < s.markets; ++market) {
        const std::int64_t action = actions[2 * market];
        const std::int64_t size = actions[2 * market + 1];
        if (action < 0 || std::cmp_greater_equal(action, s.config.actions.size()) || size < 0 ||
            std::cmp_greater_equal(size, s.config.sizes.size())) {
            throw std::invalid_argument(std::format(
                "market {}: action {} at size {}, of {} actions and {} sizes", market, action, size,
                s.config.actions.size(), s.config.sizes.size()));
        }
        if (!s.each[market].started()) {
            throw std::logic_error(std::format("market {} has no episode: reset it first", market));
        }
        if (s.each[market].finished()) {
            throw std::logic_error(
                std::format("market {}'s episode is over: reset it before stepping", market));
        }
    }
    s.forEach([&](std::size_t market) {
        s.report(market, s.each[market].step(static_cast<std::size_t>(actions[2 * market]),
                                             static_cast<std::size_t>(actions[2 * market + 1]),
                                             s.observation(market)));
    });
}

void LearningPool::stepOrders(std::span<const std::int64_t> orders, std::size_t fields) {
    State& s = *state_;
    const std::scoped_lock lock{s.calls};
    if ((fields != kOrderFields && fields != kOrderFields - 1) ||
        orders.size() != fields * s.markets) {
        throw std::invalid_argument(std::format(
            "the orders must be {} or {} numbers a market, the bid's price and size, the offer's "
            "price and size, the quantity to take and its price: {} for {} markets",
            kOrderFields - 1, kOrderFields, orders.size(), s.markets));
    }
    for (std::size_t market = 0; market < s.markets; ++market) {
        if (!s.each[market].started()) {
            throw std::logic_error(std::format("market {} has no episode: reset it first", market));
        }
        if (s.each[market].finished()) {
            throw std::logic_error(
                std::format("market {}'s episode is over: reset it before stepping", market));
        }
    }
    s.forEach([&](std::size_t market) {
        const std::int64_t* row = orders.data() + fields * market;
        s.report(market,
                 s.each[market].stepOrders({.bidPrice = row[0],
                                            .bidSize = row[1],
                                            .askPrice = row[2],
                                            .askSize = row[3],
                                            .take = row[4],
                                            .takePrice = fields == kOrderFields ? row[5] : 0},
                                           s.observation(market)));
    });
}

std::size_t LearningPool::markets() const noexcept { return state_->markets; }
std::size_t LearningPool::threads() const noexcept { return state_->workers.threads(); }
const LearningConfig& LearningPool::config() const noexcept { return state_->config; }
std::vector<std::string> LearningPool::settingNames() const { return state_->names; }

Session LearningPool::session(std::size_t market) const {
    State& s = *state_;
    if (market >= s.markets) {
        throw std::out_of_range(
            std::format("there is no market {} in a pool of {}", market, s.markets));
    }
    const std::scoped_lock lock{s.calls};
    return s.each[market].session();
}

std::span<std::int64_t> LearningPool::state() noexcept { return state_->stateBuffer; }
std::span<std::int64_t> LearningPool::orders() noexcept { return state_->orderBuffer; }
std::span<std::int64_t> LearningPool::events() noexcept { return state_->eventBuffer; }
std::span<std::int64_t> LearningPool::depth() noexcept { return state_->depthBuffer; }
std::span<std::int64_t> LearningPool::book() noexcept { return state_->bookBuffer; }
std::span<std::int64_t> LearningPool::counts() noexcept { return state_->countBuffer; }
std::span<double> LearningPool::rewards() noexcept { return state_->rewardBuffer; }
std::span<std::uint8_t> LearningPool::terminated() noexcept { return state_->terminatedBuffer; }
std::span<std::uint8_t> LearningPool::truncated() noexcept { return state_->truncatedBuffer; }
std::span<std::uint8_t> LearningPool::stopped() noexcept { return state_->stoppedBuffer; }
std::span<std::int64_t> LearningPool::elapsed() noexcept { return state_->elapsedBuffer; }
std::span<std::int64_t> LearningPool::score() noexcept { return state_->scoreBuffer; }
std::span<double> LearningPool::value() noexcept { return state_->valueBuffer; }
std::span<double> LearningPool::settings() noexcept { return state_->settingBuffer; }
std::span<std::uint64_t> LearningPool::seeds() noexcept { return state_->seedBuffer; }
std::span<std::uint64_t> LearningPool::settingsSeeds() noexcept {
    return state_->settingsSeedBuffer;
}

} // namespace crowdbook
