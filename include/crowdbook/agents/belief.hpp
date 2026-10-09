#pragma once

#include <memory>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// How a trader who watches the true value sees it. Nobody knows the value exactly: a trader sees it
// late, with an error of its own that lasts, tilted by what its group wants to be true, and with
// fresh noise on every look. Values are in ticks, or cents in a prediction market. The defaults
// see the value as it is now with fresh noise alone, as traders always have, so a scenario that
// sets none of the rest runs as it did.
struct BeliefConfig {
    double noise = 1.0; // standard deviation of the fresh error on each look
    double error = 0.0; // standard deviation of the trader's own lasting error
    // How long the lasting error lasts: its correlation with what it was this long ago is 1/e.
    Duration errorMemory = 60 * kSecond;
    double bias = 0.0; // added to every look: the group's tilt
    // How late, on average, the trader sees the value: each trader of a group is between no delay
    // and twice this, drawn once, so some hear the news first and some last.
    Duration lag = 0;

    friend bool operator==(const BeliefConfig&, const BeliefConfig&) = default;
};

// One trader's view of the value, from its own random stream.
class Belief {
public:
    // Throws std::invalid_argument without a value, for negative noise, error or lag, or an error
    // memory that is not positive.
    Belief(const BeliefConfig& config, std::shared_ptr<Fundamental> value);

    // The trader's estimate of the value now. It draws the fresh noise first, as traders always
    // have, and only then, when they are on, its delay (once) and the change in its lasting error.
    [[nodiscard]] double look(AgentContext& context);

    [[nodiscard]] const BeliefConfig& config() const noexcept { return config_; }

private:
    BeliefConfig config_;
    std::shared_ptr<Fundamental> value_;
    std::optional<Duration> delay_{};
    std::optional<double> error_{};
    Timestamp errorAt_ = 0; // when the lasting error was last brought up to date
};

} // namespace crowdbook
