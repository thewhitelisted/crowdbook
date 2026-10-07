# The trading screen's drawing, the codes it exchanges with a terminal and its model of a served
# market need no terminal and no network, so the tests link them too.
add_library(crowdbook_ladder
    crowdbook/ladder.cpp
    crowdbook/remote.cpp
    crowdbook/terminal_codes.cpp)
target_include_directories(crowdbook_ladder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/crowdbook")
target_link_libraries(crowdbook_ladder PUBLIC crowdbook::crowdbook crowdbook::protocol)
crowdbook_set_warnings(crowdbook_ladder)

add_executable(crowdbook_cli
    crowdbook/connect.cpp
    crowdbook/main.cpp
    crowdbook/output.cpp
    crowdbook/play.cpp
    crowdbook/serve.cpp
    crowdbook/terminal.cpp)
set_target_properties(crowdbook_cli PROPERTIES OUTPUT_NAME crowdbook)
target_link_libraries(crowdbook_cli PRIVATE crowdbook::gateway crowdbook_ladder)
crowdbook_set_warnings(crowdbook_cli)

if(CROWDBOOK_BUILD_TESTS)
    add_test(NAME cli.version COMMAND crowdbook_cli --version)
    set_tests_properties(cli.version PROPERTIES
        PASS_REGULAR_EXPRESSION "^crowdbook [0-9]+\\.[0-9]+\\.[0-9]+")
    add_test(NAME cli.agents COMMAND crowdbook_cli agents)
    set_tests_properties(cli.agents PROPERTIES PASS_REGULAR_EXPRESSION "market_maker")
    add_test(NAME cli.no_command COMMAND crowdbook_cli)
    add_test(NAME cli.missing_scenario COMMAND crowdbook_cli run no-such-scenario.toml)
    add_test(NAME cli.unknown_log_kind
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --log "${CMAKE_CURRENT_BINARY_DIR}/unused.csv" --log-only trades)
    set_tests_properties(cli.no_command cli.missing_scenario cli.unknown_log_kind
        PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli.json_and_filtered_log
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --json "${CMAKE_CURRENT_BINARY_DIR}/result.json"
            --log "${CMAKE_CURRENT_BINARY_DIR}/trades.csv" --log-only trade,top_of_book)
    set_tests_properties(cli.json_and_filtered_log PROPERTIES
        PASS_REGULAR_EXPRESSION "results written to")
    add_test(NAME cli.prices
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 2s --prices "${CMAKE_CURRENT_BINARY_DIR}/prices.csv"
            --sample-interval 500ms)
    set_tests_properties(cli.prices PROPERTIES PASS_REGULAR_EXPRESSION "prices written to")
    add_test(NAME cli.depth
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/maker_taker.toml"
            --duration 2s --depth "${CMAKE_CURRENT_BINARY_DIR}/depth.csv")
    set_tests_properties(cli.depth PROPERTIES PASS_REGULAR_EXPRESSION "depth written to")
    add_test(NAME cli.depth_without_feed
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --depth "${CMAKE_CURRENT_BINARY_DIR}/unused_depth.csv")
    set_tests_properties(cli.depth_without_feed PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli.fees
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/maker_taker.toml"
            --duration 2s)
    set_tests_properties(cli.fees PROPERTIES PASS_REGULAR_EXPRESSION "fees +net pnl")

    add_test(NAME cli.replay
        COMMAND crowdbook_cli replay "${PROJECT_SOURCE_DIR}/examples/sessions/playable_demo.toml"
            --log "${CMAKE_CURRENT_BINARY_DIR}/replay.csv")
    set_tests_properties(cli.replay PROPERTIES
        PASS_REGULAR_EXPRESSION "replayed [0-9.]+s with 7 actions")
    add_test(NAME cli.replay_served
        COMMAND crowdbook_cli replay "${PROJECT_SOURCE_DIR}/examples/sessions/served_demo.toml")
    set_tests_properties(cli.replay_served PROPERTIES
        PASS_REGULAR_EXPRESSION "replayed 30s with 46 actions")
    add_test(NAME cli.report
        COMMAND crowdbook_cli report "${PROJECT_SOURCE_DIR}/examples/sessions/served_demo.toml"
            --json "${CMAKE_CURRENT_BINARY_DIR}/report.json")
    set_tests_properties(cli.report PROPERTIES PASS_REGULAR_EXPRESSION "without you: last price")
    add_test(NAME cli.rewind_keeps_the_seed
        COMMAND crowdbook_cli play "${PROJECT_SOURCE_DIR}/examples/sessions/playable_demo.toml"
            --at 10s --seed 4)
    set_tests_properties(cli.rewind_keeps_the_seed PROPERTIES
        PASS_REGULAR_EXPRESSION "--at rewinds a session, which keeps its seed")
    add_test(NAME cli.replay_takes_no_seed
        COMMAND crowdbook_cli replay "${PROJECT_SOURCE_DIR}/examples/sessions/playable_demo.toml"
            --seed 3)
    # Tests run without a terminal, which play needs.
    add_test(NAME cli.play_needs_a_terminal
        COMMAND crowdbook_cli play "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml")
    set_tests_properties(cli.replay_takes_no_seed cli.play_needs_a_terminal
        PROPERTIES WILL_FAIL TRUE)

    # Mistakes are caught before anything runs or anyone plays.
    add_test(NAME cli.seed_fits_a_scenario_file
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --seed 9223372036854775808)
    set_tests_properties(cli.seed_fits_a_scenario_file PROPERTIES
        PASS_REGULAR_EXPRESSION "--seed needs a whole number from 0 to 9223372036854775807")
    add_test(NAME cli.speed_is_finite
        COMMAND crowdbook_cli play "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --speed inf)
    set_tests_properties(cli.speed_is_finite PROPERTIES
        PASS_REGULAR_EXPRESSION "--speed needs a positive number")
    add_test(NAME cli.json_path_checked_first
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --json "${CMAKE_CURRENT_BINARY_DIR}/no-such-directory/result.json")
    set_tests_properties(cli.json_path_checked_first PROPERTIES
        PASS_REGULAR_EXPRESSION "cannot write" FAIL_REGULAR_EXPRESSION "simulated")
    add_test(NAME cli.record_path_checked_first
        COMMAND crowdbook_cli play "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --record "${CMAKE_CURRENT_BINARY_DIR}/no-such-directory/session.toml")
    set_tests_properties(cli.record_path_checked_first PROPERTIES
        PASS_REGULAR_EXPRESSION "cannot write")

    # serve checks its settings before it listens.
    add_test(NAME cli.serve_listen_needs_a_port
        COMMAND crowdbook_cli serve "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --listen localhost)
    set_tests_properties(cli.serve_listen_needs_a_port PROPERTIES
        PASS_REGULAR_EXPRESSION "--listen needs host:port")
    add_test(NAME cli.serve_seat_names
        COMMAND crowdbook_cli serve "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --seat "two words")
    set_tests_properties(cli.serve_seat_names PROPERTIES
        PASS_REGULAR_EXPRESSION "is not a seat name")
    add_test(NAME cli.serve_rate_limit
        COMMAND crowdbook_cli serve "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --rate-limit 0)
    set_tests_properties(cli.serve_rate_limit PROPERTIES
        PASS_REGULAR_EXPRESSION "--rate-limit needs a whole number")
    add_test(NAME cli.serve_tokens_file
        COMMAND crowdbook_cli serve "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml"
            --tokens no-such-tokens.txt)
    set_tests_properties(cli.serve_tokens_file PROPERTIES PASS_REGULAR_EXPRESSION "cannot read")

    # The Python client's tests, its bots trading against the real server. They need Python 3.11
    # or later, and nothing else.
    find_package(Python3 3.11 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        set(client "${PROJECT_SOURCE_DIR}/clients/python")
        add_test(NAME python.client
            COMMAND Python3::Interpreter -m unittest discover -s tests
            WORKING_DIRECTORY "${client}")
        set_tests_properties(python.client PROPERTIES
            ENVIRONMENT "CROWDBOOK=$<TARGET_FILE:crowdbook_cli>;PYTHONPATH=${client}/src"
            TIMEOUT 300)
    else()
        message(STATUS "Python 3.11 or later not found: the Python client's tests are skipped")
    endif()

    # Every example scenario and challenge has to keep working: run each one briefly, with an
    # event log.
    file(GLOB challenges CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/challenges/*.toml")
    foreach(challenge IN LISTS challenges)
        get_filename_component(name "${challenge}" NAME_WE)
        add_test(NAME cli.challenge.${name}
            COMMAND crowdbook_cli run "${challenge}" --duration 2s
                --log "${CMAKE_CURRENT_BINARY_DIR}/challenge_${name}.csv")
    endforeach()
    file(GLOB examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/scenarios/*.toml")
    foreach(example IN LISTS examples)
        get_filename_component(name "${example}" NAME_WE)
        add_test(NAME cli.example.${name}
            COMMAND crowdbook_cli run "${example}" --duration 2s
                --log "${CMAKE_CURRENT_BINARY_DIR}/${name}.csv")
    endforeach()
endif()
