#!/usr/bin/env bash
# Builds the Android JNI bridge (android/app/src/main/cpp/anyshare_jni.cpp +
# portable core) for the *host* JVM and runs JniBridgeTest with -Xcheck:jni.
#
# Works on Linux (g++, .so) and on Windows Git Bash / MSYS2 (g++, .dll).
# Requires: g++ with C++17, a JDK (javac/java) with include/jni.h.
#   JAVA_HOME=/path/to/jdk tests/jni/run_jni_desktop_test.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/tests/jni/build}"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
mkdir -p "$OUT/classes"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LIB="$OUT/anyshare_jni.dll"; PLAT=win32; EXTRA="-static" ;;
    Darwin*) LIB="$OUT/libanyshare_jni.dylib"; PLAT=darwin; EXTRA="" ;;
    *) LIB="$OUT/libanyshare_jni.so"; PLAT=linux; EXTRA="-fPIC" ;;
esac

echo "== Compiling JNI bridge -> $LIB"
g++ -std=c++17 -O2 -Wall -Wextra -shared $EXTRA \
    -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/$PLAT" \
    -I"$ROOT/core-engine/include" -I"$ROOT/core-engine/third_party" \
    "$ROOT/android/app/src/main/cpp/anyshare_jni.cpp" \
    "$ROOT/core-engine/src/Protocol.cpp" "$ROOT/core-engine/src/TransferSession.cpp" \
    -o "$LIB"

echo "== Compiling Java (NativeBridge + ApduProtocol from the Android app, unmodified)"
"$JAVA_HOME/bin/javac" -d "$OUT/classes" \
    "$ROOT/android/app/src/main/java/com/example/myapplication/transfer/NativeBridge.java" \
    "$ROOT/android/app/src/main/java/com/example/myapplication/nfc/ApduProtocol.java" \
    "$ROOT/tests/jni/JniBridgeTest.java"

echo "== Running with -Xcheck:jni"
"$JAVA_HOME/bin/java" -Xcheck:jni -Djava.library.path="$OUT" -cp "$OUT/classes" JniBridgeTest
