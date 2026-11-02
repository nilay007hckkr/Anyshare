cmake_minimum_required(VERSION 3.22.1)
project(anyshare_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared C++ core from the repository root. Only the portable protocol
# library is used on Android: Java owns the Bluetooth/TCP sockets and hands
# their streams to native code, so no Linux-specific (BlueZ/libnfc) code and
# no Boost are compiled for Android.
#
# Expected layout (repo root):   <root>/android/   and   <root>/core-engine/
# Override with -DANYSHARE_CORE_DIR=/path/to/core-engine if needed.
if(NOT ANYSHARE_CORE_DIR)
    set(ANYSHARE_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core-engine)
endif()
get_filename_component(ANYSHARE_CORE_DIR "${ANYSHARE_CORE_DIR}" ABSOLUTE)
if(NOT EXISTS "${ANYSHARE_CORE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "AnyShare core engine not found at:\n  ${ANYSHARE_CORE_DIR}\n"
        "The Android app compiles the shared C++ core from the repository's core-engine/ "
        "folder, which must sit next to android/. Fix: either 'git pull' main after the "
        "core engine is merged, or unzip AnyShare-CoreEngine.zip into the same folder that "
        "contains android/. Then: File > Sync Project with Gradle Files, and "
        "Build > Refresh Linked C++ Projects.")
endif()
set(ANYSHARE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${ANYSHARE_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/core-engine)

add_library(anyshare_jni SHARED anyshare_jni.cpp)
target_link_libraries(anyshare_jni PRIVATE anyshare_protocol log)
target_compile_options(anyshare_jni PRIVATE -Wall -Wextra)
