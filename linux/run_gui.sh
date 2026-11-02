#!/usr/bin/env bash
# Builds (incrementally) and starts the AnyShare desktop app.
# The window shows everything; this terminal keeps the engine log.
#   linux/run_gui.sh                 # open the app
#   linux/run_gui.sh --receive       # open and start receiving at once
# Extra CMake flags: EXTRA_CMAKE_ARGS="-DQT_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6" linux/run_gui.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD_DIR:-$HERE/../build-linux}"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DANYSHARE_WITH_GUI=ON ${EXTRA_CMAKE_ARGS:-}
fi
cmake --build "$BUILD" -j"$(nproc)" --target anyshare-gui
# Hide Qt's harmless Wayland text-input warnings (seen on some compositors); keep other logs.
export QT_LOGGING_RULES="${QT_LOGGING_RULES:-qt.qpa.wayland.textinput=false}"
exec "$BUILD/anyshare-gui" "$@"
