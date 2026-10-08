# A project of its own that uses an installed crowdbook: CTest builds it against the package the
# build installs, to check that find_package(crowdbook) gives everything needed.
cmake_minimum_required(VERSION 3.25)
project(crowdbook_consumer LANGUAGES CXX)

find_package(crowdbook 0.9 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE crowdbook::gateway)
target_compile_features(consumer PRIVATE cxx_std_23)
