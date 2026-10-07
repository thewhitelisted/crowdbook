#pragma once

#include <cstdint>

#include "crowdbook/messages.hpp"
#include "crowdbook/simulation.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// Maps a steady wall clock to simulated time for a live session. Simulated time advances at
// `speed` times the wall clock while running, stands still while paused, and never goes
// backwards. Wall times are nanoseconds from any fixed origin.
class Pacer {
public:
    // Throws std::invalid_argument unless the speed is positive and finite.
    Pacer(std::int64_t wallNow, Timestamp simulatedNow, double speed = 1.0);

    [[nodiscard]] Timestamp simulatedAt(std::int64_t wall) const noexcept;
    // Both take effect from `wall` on, with simulated time carrying on from where it was then.
    // setSpeed throws std::invalid_argument unless the speed is positive and finite.
    void setSpeed(double speed, std::int64_t wall);
    void setPaused(bool paused, std::int64_t wall);

    [[nodiscard]] double speed() const noexcept { return speed_; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

private:
    std::int64_t wallAnchor_;
    Timestamp simulatedAnchor_;
    double speed_;
    bool paused_ = false;
};

// One thing a live participant did: a request sent at simulated time `time` from one of the
// session's seats. A new order carries the client order id it was given, so that a replay can
// check it is given the same one.
struct SessionAction {
    Timestamp time = 0;
    std::uint32_t seat = 0;       // the index of the seat in the session's list
    std::uint32_t instrument = 0; // the only instrument, until scenarios hold several
    Request request{};

    friend bool operator==(const SessionAction&, const SessionAction&) = default;
};

// Sends the action's request as `agent`, which must be due now: the simulation's clock has to be
// at the action's time. Returns the request's client order id, for a new order the one it was
// given. Throws std::logic_error if the clock is elsewhere or a new order gets a different client
// order id than the action records (when it records one), either of which means a replay has
// diverged from the session it replays.
ClientOrderId perform(Simulation& simulation, AgentId agent, const SessionAction& action);

} // namespace crowdbook
