# The trading screen's drawing involves no terminal, so the tests link it too.
add_library(crowdbook_ladder
    crowdbook/ladder.cpp)
target_include_directories(crowdbook_ladder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/crowdbook")
target_link_libraries(crowdbook_ladder PUBLIC crowdbook::crowdbook)
crowdbook_set_warnings(crowdbook_ladder)

add_executable(crowdbook_cli
    crowdbook/main.cpp
    crowdbook/output.cpp
    crowdbook/play.cpp
    crowdbook/terminal.cpp)
set_target_properties(crowdbook_cli PROPERTIES OUTPUT_NAME crowdbook)
target_link_libraries(crowdbook_cli PRIVATE crowdbook::scenario crowdbook_ladder)
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
    add_test(NAME cli.replay_takes_no_seed
        COMMAND crowdbook_cli replay "${PROJECT_SOURCE_DIR}/examples/sessions/playable_demo.toml"
            --seed 3)
    # Tests run without a terminal, which play needs.
    add_test(NAME cli.play_needs_a_terminal
        COMMAND crowdbook_cli play "${PROJECT_SOURCE_DIR}/examples/scenarios/playable.toml")
    set_tests_properties(cli.replay_takes_no_seed cli.play_needs_a_terminal
        PROPERTIES WILL_FAIL TRUE)

    # Every example scenario has to keep working: run each one briefly, with an event log.
    file(GLOB examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/scenarios/*.toml")
    foreach(example IN LISTS examples)
        get_filename_component(name "${example}" NAME_WE)
        add_test(NAME cli.example.${name}
            COMMAND crowdbook_cli run "${example}" --duration 2s
                --log "${CMAKE_CURRENT_BINARY_DIR}/${name}.csv")
    endforeach()
endif()
