#include "crowdbook/agents/belief.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "crowdbook/math.hpp"

namespace crowdbook {

Belief::Belief(const BeliefConfig& config, std::shared_ptr<Fundamental> value)
    : config_(config), value_(std::move(value)) {
    if (!value_) {
        throw std::invalid_argument(
            "a trader who watches the true value needs one: add a [fundamental] or [prediction] "
            "section");
    }
    if (!(config.noise >= 0.0) || !(config.error >= 0.0) || !std::isfinite(config.bias) ||
        config.lag < 0 || config.errorMemory <= 0) {
        throw std::invalid_argument("noise, error and lag must not be negative, the bias must be "
                                    "a number, and the error memory must be positive");
    }
    // The latest a trader of this group can see the value.
    value_->remember(2 * config.lag);
}

double Belief::look(AgentContext& context) {
    Random& random = context.random();
    const double noise = random.normal(0.0, config_.noise);
    if (config_.lag > 0 && !delay_) {
        delay_ = random.uniformInt(0, 2 * config_.lag);
    }
    const Timestamp now = context.now();
    double estimate = value_->valueAt(std::max<Timestamp>(now - delay_.value_or(0), 0)) + noise;
    if (config_.error > 0.0) {
        // The lasting error wanders back and forth around zero, an Ornstein–Uhlenbeck process
        // sampled exactly at each look.
        if (!error_) {
            error_ = random.normal(0.0, config_.error);
        } else {
            const double keep = math::exp(-static_cast<double>(now - errorAt_) /
                                          static_cast<double>(config_.errorMemory));
            *error_ = *error_ * keep +
                      config_.error * std::sqrt(1.0 - keep * keep) * random.normal(0.0, 1.0);
        }
        errorAt_ = now;
        estimate += *error_;
    }
    return estimate + config_.bias;
}

} // namespace crowdbook
