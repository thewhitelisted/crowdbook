# crowdbook as a library

The `crowdbook` command is a thin layer over libraries that any C++23 project can link: run
markets, play and serve them, score and report on sessions, all from your own program.

## Getting it

Install a build, then find the package from your project:

```bash
cmake --workflow --preset release
cmake --install build/release --prefix /opt/crowdbook
```

```cmake
find_package(crowdbook 0.9 REQUIRED)  # with CMAKE_PREFIX_PATH=/opt/crowdbook
target_link_libraries(my_program PRIVATE crowdbook::gateway)
```

Or add the source tree with `add_subdirectory` or `FetchContent`; the targets have the same names.
[examples/consumer](../examples/consumer) is a project of its own built against the installed
package in CI.

| Target | Holds | Needs |
|---|---|---|
| `crowdbook::crowdbook` | The order book, the exchange, the simulation kernel, the built-in agents, scenarios as structs, scoring | Nothing |
| `crowdbook::protocol` | The gateway's messages and their encoding as JSON lines | `crowdbook` |
| `crowdbook::scenario` | Scenario and session files, live sessions, replays, rewinding, reports | `crowdbook`; toml++ is compiled in |
| `crowdbook::gateway` | The gateway, which serves a market to clients over bytes, and the POSIX socket server around it | `scenario`, `protocol` |

## What to use for what

| To | Use | Header |
|---|---|---|
| Run a market to the end | `runScenario`, or `ScenarioRun` to run it in steps and add agents of your own | `scenario.hpp` |
| Write an agent | Subclass `Agent`; register it in an `AgentRegistry` to name it in scenario files | `agent.hpp`, `agent_registry.hpp` |
| Read scenario and session files | `parseScenario`, `loadScenario`, `parseSession`, `loadSession`, `writeSession` | `scenario_file.hpp`, `session.hpp` |
| Put people or programs in a market you drive | `openSession`, then `Simulation::act` or `perform` between calls to `runUntil` | `session.hpp`, `live.hpp` |
| Replay, leave seats out, or rewind | `replaySession`, `rewindSession` | `session.hpp` |
| Serve a market over your own transport | `Gateway`: pass in bytes and the wall clock, take out bytes; it never touches a socket | `gateway.hpp` |
| Serve it over TCP | `serve`, around a `Gateway` | `server.hpp` |
| Speak the protocol as a client | `protocol::encode`, `protocol::decodeServer` | `protocol.hpp` |
| Score sessions | `Scorer`, an `EventSink`; sessions of scenarios with scoring do it for you | `scoring.hpp` |
| Report on a session | `makeReport`, `writeReportJson` | `report.hpp` |
| Record what happens | `CsvEventLog`, `PriceSampler`, `DepthSampler`, or your own `EventSink` | `event_log.hpp` |

A hosted service would hold one `Gateway` per market, feed it the bytes its own connections
deliver, whether WebSocket or anything else, and call `advance` on a timer. Everything
happens on the thread that calls it, so markets can run on as many threads as there are cores,
one market to a thread.

## Versions

The library follows semantic versioning. Until 1.0, a minor version may change the API, so the
package only matches its own minor version; from 1.0, only a major version will.

Formats carry their own version numbers, which change only when the format does:

| Format | Version | Where | Compatibility |
|---|---|---|---|
| Protocol | `protocol` in `hello` and `welcome` | 1 | A server refuses a client of another protocol version |
| Scenario files | `scenario_version`, optional | 1 | A file for a newer crowdbook is refused rather than misread |
| Session files | `session_version`; `recorded_by` names the crowdbook that recorded it | 2 | Every earlier version still reads |

A session replays exactly only on a crowdbook whose markets behave the same way. CI replays
sessions recorded by earlier versions and pins their event logs and scores, so a change that
would alter them is a decision, recorded in the tests, rather than an accident. A replay that
diverges stops with an error, which names the crowdbook that recorded the session when it was
another.

## Capacity

The markets of the example challenges, sized for a person to trade in, run on one core of an
Apple M5 at:

| | Times real time |
|---|---:|
| The market alone | 20,000 to 28,000 |
| Served through the gateway to one client reading every message | 4,600 to 5,800 |

So one core keeps up with thousands of such markets in computation; encoding market data for
clients costs about four fifths of it. A served market also spends a system call or two per
connection per millisecond in its socket loop, which these figures leave out. To measure:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench --benchmark_filter=Challenge
```

## Containers

The [Dockerfile](../Dockerfile) builds an image holding the `crowdbook` command and the examples:

```bash
docker build -t crowdbook .
docker run --rm -p 7878:7878 crowdbook serve \
    /usr/local/share/crowdbook/examples/challenges/market_making.toml --listen :7878
```

CI builds the image and runs a market in it.
