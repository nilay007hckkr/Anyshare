cmake_minimum_required(VERSION 3.10)
project(CoreEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANYSHARE_BUILD_TESTS "Build demo, stress test and unit/integration tests" ON)

find_package(Threads REQUIRED)

# nlohmann/json: system package if present, otherwise the vendored single
# header (third_party/, v3.11.3, brought over from the Android branch) so the
# Android NDK build needs no extra dependency.
find_package(nlohmann_json 3.2 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    add_library(anyshare_nlohmann_json INTERFACE)
    target_include_directories(anyshare_nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    add_library(nlohmann_json::nlohmann_json ALIAS anyshare_nlohmann_json)
endif()

# Portable protocol core (ASP/1 framing, sender/receiver, checksums, path
# safety). No OS APIs: used by Linux, the Android JNI bridge and Windows tests.
add_library(anyshare_protocol STATIC
    src/Protocol.cpp
    src/TransferSession.cpp
)
target_include_directories(anyshare_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyshare_protocol PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(anyshare_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(anyshare_protocol PRIVATE -Wall -Wextra)
endif()

if(NOT WIN32 AND NOT ANDROID)
    # POSIX sockets + the sender orchestration (connect/retry/resume).
    # Not needed on Android, where Java owns the sockets.
    add_library(core_engine STATIC
        src/SocketStream.cpp
        src/NetworkEngine.cpp
    )
    target_include_directories(core_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(core_engine PUBLIC anyshare_protocol)
    set_target_properties(core_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(core_engine PRIVATE -Wall -Wextra)

    if(ANYSHARE_BUILD_TESTS)
        add_executable(demo test/main.cpp)
        target_link_libraries(demo PRIVATE core_engine)

        add_executable(stress_test test/stress_test.cpp)
        target_link_libraries(stress_test PRIVATE core_engine)

        add_executable(receiver test/receiver.cpp)
        target_link_libraries(receiver PRIVATE core_engine)

        add_executable(core_tests test/core_tests.cpp)
        target_link_libraries(core_tests PRIVATE core_engine)

        enable_testing()
        add_test(NAME core_tests COMMAND core_tests)
    endif()
endif()
