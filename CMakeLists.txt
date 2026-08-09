# ===========================================================================
# CMakeLists.txt — AnyShare Network Engine
# ===========================================================================
# Build targets:
#   1. anyshare_engine  (STATIC library)  — the cross-platform C++17 engine
#   2. anyshare_demo    (executable)      — local test harness
#
# Usage (from project root):
#   mkdir -p build && cd build
#   cmake ..
#   make -j$(nproc)
#   ./anyshare_demo [path_to_file]
#
# To link from an Android NDK project or another CMake project:
#   add_subdirectory(path/to/Anyshare)
#   target_link_libraries(your_target PRIVATE anyshare_engine)
# ===========================================================================

cmake_minimum_required(VERSION 3.10)
project(AnyShare
    VERSION     1.0.0
    LANGUAGES   CXX
    DESCRIPTION "Cross-platform async file transfer engine for Wi-Fi Direct"
)

# ---------------------------------------------------------------------------
# C++17 Standard (required for std::optional, structured bindings, etc.)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)        # No GNU extensions — pure C++17

# ---------------------------------------------------------------------------
# Find Boost (Boost.Asio requires Boost::system on older Boost versions)
# ---------------------------------------------------------------------------
find_package(Boost 1.74 REQUIRED COMPONENTS system)

# ---------------------------------------------------------------------------
# Find Threads (required by Boost.Asio for async operations)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Target 1: anyshare_engine (STATIC library)
# ---------------------------------------------------------------------------
# OOP Principle: ENCAPSULATION at the build level — the engine is packaged
# as a self-contained library with a clean public header interface.
# Consumers only see include/ headers; src/ implementation is private.
# ---------------------------------------------------------------------------
add_library(anyshare_engine STATIC
    src/NetworkEngine.cpp
    src/JsonHandover.cpp
)

# Public headers: consumers of the library get access to include/ and third_party/
target_include_directories(anyshare_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Link dependencies
target_link_libraries(anyshare_engine
    PUBLIC
        Boost::system
        Threads::Threads
)

# Compiler warnings — strict but not pedantic (academic project, not production)
target_compile_options(anyshare_engine PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
)
