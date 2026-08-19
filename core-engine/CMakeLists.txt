cmake_minimum_required(VERSION 3.10)
project(CoreEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(core_engine STATIC
    src/NetworkEngine.cpp
)

target_include_directories(core_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(core_engine PUBLIC
    Boost::system
    Threads::Threads
    nlohmann_json::nlohmann_json
)

add_executable(demo
    test/main.cpp
)

target_link_libraries(demo PRIVATE
    core_engine
)

add_executable(stress_test
    test/stress_test.cpp
)

target_link_libraries(stress_test PRIVATE
    core_engine
)
