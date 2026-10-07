add_library(crowdbook
    agents/informed.cpp
    agents/market_maker.cpp
    agents/market_view.cpp
    agents/momentum.cpp
    agents/zero_intelligence.cpp
    event_log.cpp
    exchange.cpp
    fundamental.cpp
    ledger.cpp
    messages.cpp
    order_book.cpp
    parameters.cpp
    random.cpp
    simulation.cpp
    types.cpp
    version.cpp)
add_library(crowdbook::crowdbook ALIAS crowdbook)

target_include_directories(crowdbook PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(crowdbook PUBLIC cxx_std_23)
target_compile_definitions(crowdbook PRIVATE CROWDBOOK_VERSION="${PROJECT_VERSION}")
crowdbook_set_warnings(crowdbook)
