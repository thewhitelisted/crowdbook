// crowdbook's learning pool for Python: the C++ pool, with its buffers as NumPy arrays that view
// the pool's own memory. Stepping and resetting release the interpreter's lock while the markets
// run on the pool's threads.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "crowdbook/learning.hpp"
#include "crowdbook/session.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using crowdbook::LearningPool;

using Seeds = nb::ndarray<const std::uint64_t, nb::ndim<1>, nb::c_contig, nb::device::cpu>;
using Mask = nb::ndarray<const std::uint8_t, nb::ndim<1>, nb::c_contig, nb::device::cpu>;
using Actions = nb::ndarray<const std::int64_t, nb::ndim<2>, nb::c_contig, nb::device::cpu>;

// A read-only NumPy array over memory the pool owns, which keeps the pool alive.
template <typename T>
nb::object view(nb::handle pool, const T* data, std::initializer_list<std::size_t> shape) {
    return nb::ndarray<nb::numpy, const T>(data, shape, pool).cast();
}

template <typename T>
std::span<const T> span(const nb::ndarray<const T, nb::ndim<1>, nb::c_contig, nb::device::cpu>& array) {
    return {array.data(), array.shape(0)};
}

nb::dict buffers(nb::handle self) {
    LearningPool& pool = nb::cast<LearningPool&>(self);
    const crowdbook::LearningConfig& config = pool.config();
    const std::size_t n = pool.markets();
    const std::size_t levels = config.depthLevels;
    nb::dict out;
    out["state"] = view(self, pool.state().data(), {n, crowdbook::kLearningStateNames.size()});
    out["orders"] = view(self, pool.orders().data(), {n, config.maxOrders, crowdbook::kOrderColumns});
    out["events"] = view(self, pool.events().data(), {n, config.maxEvents, crowdbook::kEventColumns});
    out["depth"] = view(self, pool.depth().data(), {n, config.maxDepthUpdates, 2, levels, 2});
    out["book"] = view(self, pool.book().data(), {n, 2, levels, 2});
    out["counts"] = view(self, pool.counts().data(), {n, crowdbook::kCountColumns});
    out["rewards"] = view(self, pool.rewards().data(), {n});
    // One byte each, 0 or 1, the same in memory as NumPy's booleans.
    out["terminated"] = view(self, reinterpret_cast<const bool*>(pool.terminated().data()), {n});
    out["truncated"] = view(self, reinterpret_cast<const bool*>(pool.truncated().data()), {n});
    out["stopped"] = view(self, reinterpret_cast<const bool*>(pool.stopped().data()), {n});
    out["elapsed"] = view(self, pool.elapsed().data(), {n});
    out["score"] = view(self, pool.score().data(), {n});
    out["value"] = view(self, pool.value().data(), {n});
    out["settings"] = view(self, pool.settings().data(), {n, pool.settingNames().size()});
    out["seeds"] = view(self, pool.seeds().data(), {n});
    out["settings_seeds"] = view(self, pool.settingsSeeds().data(), {n});
    return out;
}

void reset(LearningPool& pool, const Seeds& seeds, const std::optional<Seeds>& settingsSeeds,
           const std::optional<Mask>& mask,
           const std::optional<std::vector<std::optional<std::string>>>& scenarios) {
    const std::span<const std::optional<std::string>> own =
        scenarios ? std::span<const std::optional<std::string>>{*scenarios}
                  : std::span<const std::optional<std::string>>{};
    pool.reset(span(seeds), settingsSeeds ? span(*settingsSeeds) : std::span<const std::uint64_t>{},
               mask ? span(*mask) : std::span<const std::uint8_t>{}, own);
}

void step(LearningPool& pool, const Actions& actions) {
    if (actions.shape(1) != 2) {
        throw std::invalid_argument("the actions must have two columns, the action and the size");
    }
    pool.step({actions.data(), actions.shape(0) * 2});
}

std::string session(const LearningPool& pool, std::size_t market) {
    std::ostringstream out;
    crowdbook::writeSession(out, pool.session(market));
    return out.str();
}

nb::dict config(const LearningPool& pool) {
    const crowdbook::LearningConfig& c = pool.config();
    nb::dict out;
    out["interval_ns"] = c.interval;
    out["warmup_ns"] = c.warmup;
    out["take_ticks"] = c.takeTicks;
    out["max_events"] = c.maxEvents;
    out["max_depth_updates"] = c.maxDepthUpdates;
    out["depth_levels"] = c.depthLevels;
    out["max_orders"] = c.maxOrders;
    out["feed"] = c.feed.has_value();
    return out;
}

std::vector<std::string> actionNames(const LearningPool& pool) {
    std::vector<std::string> names;
    for (const crowdbook::LearningAction action : pool.config().actions) {
        names.emplace_back(crowdbook::toString(action));
    }
    return names;
}

std::vector<std::string> stateNames() {
    return {crowdbook::kLearningStateNames.begin(), crowdbook::kLearningStateNames.end()};
}

std::vector<std::string> eventNames() {
    std::vector<std::string> names;
    for (std::int64_t type = 0; type <= static_cast<std::int64_t>(crowdbook::LearningEvent::Feed);
         ++type) {
        names.emplace_back(crowdbook::toString(static_cast<crowdbook::LearningEvent>(type)));
    }
    return names;
}

} // namespace

NB_MODULE(_core, module) {
    module.doc() = "crowdbook's learning pool: many markets stepped at once on threads of their own";

    nb::exception<crowdbook::ScenarioError>(module, "ScenarioError", PyExc_ValueError);

    nb::class_<LearningPool>(module, "Pool")
        .def(nb::init<std::string, std::string, std::size_t, std::size_t>(), "text"_a,
             "source"_a, "markets"_a, "threads"_a = 0,
             "Builds `markets` markets of the scenario in `text`, stepped on `threads` threads, "
             "0 for every core.")
        .def("reset", &reset, "seeds"_a, "settings_seeds"_a = nb::none(), "mask"_a = nb::none(),
             "scenarios"_a = nb::none(), nb::call_guard<nb::gil_scoped_release>(),
             "Starts a new episode in each market the mask selects, or in every market.")
        .def("step", &step, "actions"_a, nb::call_guard<nb::gil_scoped_release>(),
             "Takes each market's action, a row of the action's number and the size's, and runs "
             "every market to its next decision.")
        .def("buffers", &buffers, "The pool's buffers as read-only NumPy arrays over its memory.")
        .def("session", &session, "market"_a,
             "Market `market`'s episode so far, as a session file crowdbook can replay.")
        .def_prop_ro("markets", &LearningPool::markets)
        .def_prop_ro("threads", &LearningPool::threads)
        .def_prop_ro("action_names", &actionNames)
        .def_prop_ro("sizes", [](const LearningPool& pool) { return pool.config().sizes; })
        .def_prop_ro("setting_names", &LearningPool::settingNames)
        .def_prop_ro("config", &config);

    module.attr("STATE_NAMES") = stateNames();
    module.attr("EVENT_NAMES") = eventNames();
    module.attr("EVENT_COLUMNS") = crowdbook::kEventColumns;
    module.attr("ORDER_COLUMNS") = crowdbook::kOrderColumns;
}
