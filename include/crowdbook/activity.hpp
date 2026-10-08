#pragma once

#include "crowdbook/types.hpp"

namespace crowdbook {

// How busy a trading day is expected to be through the day: a U shape, busiest at the open and
// the close and quietest at midday, as on real exchanges. at(t) multiplies a rate: it is
// (1 + amplitude · u) / (1 + amplitude / 3), where u = (2t / day - 1)² runs from 1 at the open
// to 0 at midday and back to 1 at the close, so the day as a whole keeps its average rate. An
// amplitude of 0, or no day, is a flat 1.
struct ActivityCurve {
    double amplitude = 0.0;
    Duration day = 0;

    [[nodiscard]] double at(Timestamp time) const noexcept {
        if (amplitude == 0.0 || day <= 0) {
            return 1.0;
        }
        const double x = time <= 0 ? 0.0
                         : time >= day ? 1.0
                                       : static_cast<double>(time) / static_cast<double>(day);
        const double u = (2.0 * x - 1.0) * (2.0 * x - 1.0);
        return (1.0 + amplitude * u) / (1.0 + amplitude / 3.0);
    }
};

} // namespace crowdbook
