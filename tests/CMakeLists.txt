include(FetchContent)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    SYSTEM)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

add_executable(crowdbook_tests
    event_log_test.cpp
    exchange_random_test.cpp
    exchange_test.cpp
    ledger_test.cpp
    order_book_differential_test.cpp
    order_book_test.cpp
    random_test.cpp
    simulation_determinism_test.cpp
    simulation_test.cpp
    version_test.cpp)
target_link_libraries(crowdbook_tests PRIVATE crowdbook::crowdbook GTest::gtest_main)
crowdbook_set_warnings(crowdbook_tests)

include(GoogleTest)
gtest_discover_tests(crowdbook_tests)
