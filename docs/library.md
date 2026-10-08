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
find_package(crowdbook 0.10 REQUIRED)  # with CMAKE_PREFIX_PATH=/opt/crowdbook
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
| `crowdbook::gateway` | The gateway, which serves a market to clients over bytes, and the socket server around it, on epoll (Linux) or kqueue (macOS and the BSDs) | `scenario`, `protocol` |

## What to use for what

| To | Use | Header |
|---|---|---|
| Run a market to the end | `runScenario`, or `ScenarioRun` to run it in steps and add agents of your own | `scenario.hpp` |
| Write an agent | Subclass `Agent`; register it in an `AgentRegistry` to name it in scenario files. Keep prices within 1 to `context.maxPrice()` | `agent.hpp`, `agent_registry.hpp` |
| Follow a scenario's true value, or a prediction market's probability | `makeFundamental` | `scenario.hpp`, `fundamental.hpp` |
| Read scenario and session files | `parseScenario`, `loadScenario`, `parseSession`, `loadSession`, `writeSession` | `scenario_file.hpp`, `session.hpp` |
| Put people or programs in a market you drive | `openSession`, then `Simulation::act` or `perform` between calls to `runUntil` | `session.hpp`, `live.hpp` |
| Replay, leave seats out, or rewind | `replaySession`, `rewindSession` | `session.hpp` |
| Serve a market over your own transport | `Gateway`: pass in bytes and the wall clock, take out bytes; it never touches a socket | `gateway.hpp` |
| Serve it over TCP | `serve`, around a `Gateway` | `server.hpp` |
| Speak the protocol as a client | `protocol::encode` or `encodeTo`, `protocol::decodeServer` | `protocol.hpp` |
| Score sessions | `Scorer`, an `EventSink`; sessions of scenarios with scoring do it for you | `scoring.hpp` |
| Report on a session | `makeReport`, on as many threads as you give it; `writeReportJson` | `report.hpp` |
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
| The market alone | 29,000 to 43,000 |
| Served through the gateway to one client reading every message | 19,000 to 27,000 |

So one core keeps up with thousands of such markets in computation; encoding market data for
clients costs about two fifths of it. Each served market takes about 400 KB of memory, so a
gigabyte holds some 2,500. To measure:

```bash
cmake --workflow --preset release
./build/release/bench/crowdbook_bench --benchmark_filter='Challenge|MemoryPerMarket'
```

## Latency under load

`crowdbook_load` serves a market over TCP to many clients, each placing an order or cancelling
it 20 times a second at random moments while reading all the market data, and times every
answer. The playable market's seats are a millisecond from the exchange each way, so an answer is
due 2 ms after its order; the table gives how much later it arrives, which is what the server and
the operating system add. On an Apple M5:

| Clients | Market data | Late at the median | 99th percentile | Server CPU |
|---:|---|---:|---:|---:|
| 1 | as it happens | 0.24 ms | 0.34 ms | 1% |
| 1 | as it happens, `--spin 150us` | 0.14 ms | 0.21 ms | 1% |
| 50 | as it happens | 0.17 ms | 0.54 ms | 18% |
| 200 | as it happens | 1.3 ms | 2.6 ms | 79% |
| 200 | every 10 ms | 0.18 ms | 1.1 ms | 28% |

On CI's Linux machine, four shared cores of an AMD EPYC 7763 that the clients run on too, one
client's answers are 0.15 ms late at the median and 0.23 ms at the 99th percentile without any
spin, 50 clients' 0.19 and 0.83 ms, and 200 clients' with market data every 10 ms 0.5 and 3.2 ms;
each run shows the latest figures.

A bare TCP round trip between two threads on the same machine takes 35 to 80 µs, depending on how
deeply its cores sleep; most of what remains for one client is that. With hundreds of seats, the
cost is the market data: every seat gets every update, so batching it (`--feed-interval`) is
what lets one core serve them. To measure, against a server of its own or one already running:

```bash
./build/release/bench/crowdbook_load --clients 50 --seconds 10
./build/release/bench/crowdbook_load --clients 200 --feed-interval 10000
```

## Containers

The [Dockerfile](../Dockerfile) builds an image holding the `crowdbook` command and the examples:

```bash
docker build -t crowdbook .
docker run --rm -p 7878:7878 crowdbook serve \
    /usr/local/share/crowdbook/examples/challenges/market_making.toml --listen :7878
```

CI builds the image and runs a market in it.
