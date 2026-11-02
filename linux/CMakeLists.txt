cmake_minimum_required(VERSION 3.10)
project(AnyShareLinux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Hardware backends are optional and auto-detected. Force with
# -DANYSHARE_WITH_BLUEZ=ON / -DANYSHARE_WITH_LIBNFC=ON (fails if missing).
set(ANYSHARE_WITH_BLUEZ AUTO CACHE STRING "Bluetooth RFCOMM via BlueZ (AUTO/ON/OFF)")
set(ANYSHARE_WITH_LIBNFC AUTO CACHE STRING "NFC (PN532) via libnfc (AUTO/ON/OFF)")
set(ANYSHARE_WITH_GUI AUTO CACHE STRING "Qt desktop app anyshare-gui (AUTO/ON/OFF)")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core-engine ${CMAKE_CURRENT_BINARY_DIR}/core-engine)

find_package(PkgConfig QUIET)

# find_optional_lib(<VAR> <pkg-config name> <header> <library>)
# Defines imported target anyshare_<VAR> and <VAR>_FOUND. Uses pkg-config when
# available, otherwise plain find_path/find_library (honours CMAKE_PREFIX_PATH).
function(find_optional_lib var pcname header library)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(${var} QUIET IMPORTED_TARGET ${pcname})
        if(${var}_FOUND)
            add_library(anyshare_${var} INTERFACE IMPORTED GLOBAL)
            set_target_properties(anyshare_${var} PROPERTIES INTERFACE_LINK_LIBRARIES PkgConfig::${var})
            set(${var}_FOUND TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    find_path(${var}_INCLUDE_DIR ${header})
    find_library(${var}_LIBRARY ${library})
    if(${var}_INCLUDE_DIR AND ${var}_LIBRARY)
        add_library(anyshare_${var} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(anyshare_${var} PROPERTIES
            IMPORTED_LOCATION ${${var}_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${${var}_INCLUDE_DIR})
        set(${var}_FOUND TRUE PARENT_SCOPE)
    else()
        set(${var}_FOUND FALSE PARENT_SCOPE)
    endif()
endfunction()

# Hardware backends shared by the CLI and the GUI (sources are compiled into each).
add_library(anyshare_platform INTERFACE)
target_include_directories(anyshare_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anyshare_platform INTERFACE core_engine)
target_sources(anyshare_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/Esp32Bridge.cpp    # ESP32 OLED display link
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src/LanDiscovery.cpp)  # find receivers by name

add_executable(anyshare src/main.cpp)
target_link_libraries(anyshare PRIVATE anyshare_platform)
target_compile_options(anyshare PRIVATE -Wall -Wextra)

if(NOT ANYSHARE_WITH_BLUEZ STREQUAL "OFF")
    find_optional_lib(BLUEZ bluez bluetooth/rfcomm.h bluetooth)
    if(BLUEZ_FOUND)
        target_sources(anyshare_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/BluetoothRfcomm.cpp)
        target_link_libraries(anyshare_platform INTERFACE anyshare_BLUEZ)
        target_compile_definitions(anyshare_platform INTERFACE ANYSHARE_HAVE_BLUEZ)
        message(STATUS "AnyShare: Bluetooth RFCOMM enabled (BlueZ)")
    elseif(ANYSHARE_WITH_BLUEZ STREQUAL "ON")
        message(FATAL_ERROR "BlueZ not found: sudo apt install libbluetooth-dev")
    else()
        message(STATUS "AnyShare: Bluetooth disabled (libbluetooth-dev not found)")
    endif()
endif()

if(NOT ANYSHARE_WITH_LIBNFC STREQUAL "OFF")
    find_optional_lib(LIBNFC libnfc nfc/nfc.h nfc)
    if(LIBNFC_FOUND)
        target_sources(anyshare_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/Pn532NfcHandover.cpp)
        target_link_libraries(anyshare_platform INTERFACE anyshare_LIBNFC)
        target_compile_definitions(anyshare_platform INTERFACE ANYSHARE_HAVE_LIBNFC)
        message(STATUS "AnyShare: NFC enabled (libnfc)")
    elseif(ANYSHARE_WITH_LIBNFC STREQUAL "ON")
        message(FATAL_ERROR "libnfc not found: sudo apt install libnfc-dev")
    else()
        message(STATUS "AnyShare: NFC disabled (libnfc-dev not found)")
    endif()
endif()

install(TARGETS anyshare RUNTIME DESTINATION bin)

# Discovery tests (loopback UDP).
add_executable(discovery_tests test/discovery_tests.cpp)
target_link_libraries(discovery_tests PRIVATE anyshare_platform)
target_compile_options(discovery_tests PRIVATE -Wall -Wextra)

# ESP32 bridge protocol tests (a pseudo-terminal plays the ESP32).
add_executable(bridge_tests test/bridge_tests.cpp)
target_link_libraries(bridge_tests PRIVATE anyshare_platform util)
target_compile_options(bridge_tests PRIVATE -Wall -Wextra)

# Qt desktop app (Qt 6, or Qt 5.12+). Optional: the CLI never needs Qt.
if(NOT ANYSHARE_WITH_GUI STREQUAL "OFF")
    find_package(QT NAMES Qt6 Qt5 QUIET COMPONENTS Widgets)
    if(QT_FOUND)
        find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS Widgets OPTIONAL_COMPONENTS Test)
    endif()
    if(TARGET Qt${QT_VERSION_MAJOR}::Widgets AND QT_VERSION VERSION_GREATER_EQUAL 5.12)
        add_library(anyshare_gui_core STATIC gui/MainWindow.cpp gui/Widgets.cpp)
        target_include_directories(anyshare_gui_core PUBLIC gui)
        target_link_libraries(anyshare_gui_core PUBLIC anyshare_platform Qt${QT_VERSION_MAJOR}::Widgets)
        target_compile_options(anyshare_gui_core PRIVATE -Wall -Wextra)

        add_executable(anyshare-gui gui/main.cpp)
        target_link_libraries(anyshare-gui PRIVATE anyshare_gui_core)
        install(TARGETS anyshare-gui RUNTIME DESTINATION bin)
        message(STATUS "AnyShare: Qt ${QT_VERSION} desktop app enabled (anyshare-gui)")

        if(TARGET Qt${QT_VERSION_MAJOR}::Test)
            add_executable(anyshare-gui-tests gui/test/gui_tests.cpp)
            target_link_libraries(anyshare-gui-tests PRIVATE anyshare_gui_core Qt${QT_VERSION_MAJOR}::Test util)
            target_include_directories(anyshare-gui-tests PRIVATE test)
            set_target_properties(anyshare-gui-tests PROPERTIES AUTOMOC ON)
        endif()
    elseif(ANYSHARE_WITH_GUI STREQUAL "ON")
        message(FATAL_ERROR "Qt Widgets not found: sudo apt install qt6-base-dev (Ubuntu) / sudo pacman -S qt6-base (Arch)")
    else()
        message(STATUS "AnyShare: desktop app disabled (Qt Widgets dev package not found)")
    endif()
endif()
