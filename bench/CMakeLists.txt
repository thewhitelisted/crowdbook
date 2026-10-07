include(FetchContent)

FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    SYSTEM)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_WERROR OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_INSTALL_DOCS OFF)
FetchContent_MakeAvailable(benchmark)

add_executable(crowdbook_bench
    order_book_bench.cpp
    simulation_bench.cpp)
target_link_libraries(crowdbook_bench PRIVATE crowdbook::crowdbook benchmark::benchmark_main)
crowdbook_set_warnings(crowdbook_bench)

if(CROWDBOOK_BUILD_TESTS)
    # Runs each benchmark once so a broken benchmark fails the build; real numbers come from
    # running crowdbook_bench directly on a quiet machine.
    add_test(NAME bench.smoke COMMAND crowdbook_bench --benchmark_min_time=1x)
endif()
