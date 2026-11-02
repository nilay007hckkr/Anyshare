#!/usr/bin/env bash
# Builds core-engine + the Linux CLI and runs every test that needs no
# Bluetooth/NFC hardware: core unit/integration tests and CLI end-to-end
# Linux<->Linux transfers over loopback TCP.
#
#   tests/run_linux_tests.sh [BUILD_DIR]      (default /tmp/anyshare-build)
#   EXTRA_CMAKE_ARGS="-DANYSHARE_WITH_BLUEZ=ON -DANYSHARE_WITH_LIBNFC=ON" tests/run_linux_tests.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-/tmp/anyshare-build}"
WORK="$(mktemp -d /tmp/anyshare-e2e.XXXXXX)"
PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
cleanup() { [ -n "${RX_PID:-}" ] && kill "$RX_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

echo "== Build ($BUILD)"
# shellcheck disable=SC2086
cmake -S "$ROOT/linux" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release ${EXTRA_CMAKE_ARGS:-} > "$WORK/cmake.log" 2>&1 || { cat "$WORK/cmake.log"; exit 1; }
grep "AnyShare:" "$WORK/cmake.log" | sed 's/^-- /  /'
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/build.log" 2>&1 || { grep -E "error" "$WORK/build.log"; exit 1; }
if grep -q "warning:" "$WORK/build.log"; then grep "warning:" "$WORK/build.log"; bad "build has warnings"; else ok "build without warnings"; fi
CLI="$BUILD/anyshare"
CORE="$BUILD/core-engine"

echo "== Core unit + integration tests"
if "$CORE/core_tests" > "$WORK/core.log" 2>&1; then ok "core_tests ($(tail -1 "$WORK/core.log"))"; else cat "$WORK/core.log"; bad "core_tests"; fi
grep "MiB/s" "$WORK/core.log"

wait_for_file() { for _ in $(seq 1 50); do [ -s "$1" ] && return 0; sleep 0.1; done; return 1; }
start_rx() {  # args passed to `anyshare receive`; stdin is not a TTY
    rm -f "$WORK/handover.json"
    "$CLI" receive --bind 127.0.0.1 --port 0 --once --handover-out "$WORK/handover.json" "$@" \
        > "$WORK/rx.log" 2>&1 < /dev/null &
    RX_PID=$!
    wait_for_file "$WORK/handover.json"
}
port_of() { python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["port"])' "$WORK/handover.json"; }

mkdir -p "$WORK/src" "$WORK/inbox"
head -c 5000000 /dev/urandom > "$WORK/src/photo.jpg"
: > "$WORK/src/empty.txt"
printf 'hello anyshare\n' > "$WORK/src/notes.txt"

echo "== Linux -> Linux, simulated tap (handover file, token auto-accept)"
start_rx --dir "$WORK/inbox"
"$CLI" send --handover-file "$WORK/handover.json" --name e2e-sender \
    "$WORK/src/photo.jpg" "$WORK/src/empty.txt" "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1
TX=$?; wait "$RX_PID"; RX=$?; RX_PID=
check "sender exit 0" "[ $TX -eq 0 ]"
check "receiver exit 0" "[ $RX -eq 0 ]"
check "sender reports simulated tap (no NFC claimed)" "grep -q 'Handover: simulated tap' '$WORK/tx.log'"
check "receiver authorised by tap token" "grep -q 'authorised by tap token' '$WORK/rx.log'"
check "photo.jpg identical" "cmp -s '$WORK/src/photo.jpg' '$WORK/inbox/photo.jpg'"
check "empty.txt identical" "[ -f '$WORK/inbox/empty.txt' ] && [ ! -s '$WORK/inbox/empty.txt' ]"
check "notes.txt identical" "cmp -s '$WORK/src/notes.txt' '$WORK/inbox/notes.txt'"

echo "== Same files again: existing files must not be overwritten"
start_rx --dir "$WORK/inbox"
"$CLI" send --handover-file "$WORK/handover.json" "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"; RX_PID=
check "second copy saved as 'notes (1).txt'" "[ $TX -eq 0 ] && cmp -s '$WORK/src/notes.txt' '$WORK/inbox/notes (1).txt'"

echo "== Manual address without token: receiver with no TTY must decline"
start_rx --dir "$WORK/inbox2"
"$CLI" send --tcp "127.0.0.1:$(port_of)" "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"; RX_PID=
check "sender fails with 'declined'" "[ $TX -ne 0 ] && grep -q declined '$WORK/tx.log'"
check "nothing written" "[ ! -e '$WORK/inbox2/notes.txt' ]"

echo "== Manual address with --yes receiver"
start_rx --dir "$WORK/inbox3" --yes
"$CLI" send --tcp "127.0.0.1:$(port_of)" "$WORK/src/photo.jpg" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"; RX_PID=
check "transfer verified" "[ $TX -eq 0 ] && cmp -s '$WORK/src/photo.jpg' '$WORK/inbox3/photo.jpg'"
check "sender labels manual address (no NFC)" "grep -q 'Handover: manual address (no NFC)' '$WORK/tx.log'"

echo "== NFC requested but hardware unavailable -> explicit fallback"
start_rx --dir "$WORK/inbox4" --yes
"$CLI" send --nfc --nfc-device "pn532_uart:/dev/ttyNONEXISTENT" --fallback-tcp "127.0.0.1:$(port_of)" \
    "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"; RX_PID=
check "fallback transfer verified" "[ $TX -eq 0 ] && cmp -s '$WORK/src/notes.txt' '$WORK/inbox4/notes.txt'"
check "log says NFC was NOT used" "grep -qiE 'fallback|not compiled' '$WORK/tx.log'"
check "log never claims a physical tap" "! grep -q 'Handover: physical NFC' '$WORK/tx.log'"
"$CLI" send --nfc --nfc-device "pn532_uart:/dev/ttyNONEXISTENT" "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
check "NFC without fallback fails clearly" "[ $TX -ne 0 ]"
"$CLI" nfc-probe "pn532_uart:/dev/ttyNONEXISTENT" > "$WORK/probe.log" 2>&1; TX=$?
check "nfc-probe reports hardware unavailable" "[ $TX -ne 0 ] && grep -qiE 'not available|not compiled' '$WORK/probe.log'"

echo "== Android-receiver payload -> Linux sender (handover format compatibility)"
# Exact shape produced by TransferController.startReceiving() on Android:
# Bluetooth name (Android cannot publish its MAC) + Wi-Fi alternative + token.
start_rx --dir "$WORK/inbox5"
TOKEN=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["token"])' "$WORK/handover.json")
printf '{"v":1,"transport":"rfcomm","name":"Pixel 7","ip":"127.0.0.1","port":%s,"token":"%s"}' \
    "$(port_of)" "$TOKEN" > "$WORK/android_payload.json"
"$CLI" send --handover-file "$WORK/android_payload.json" --prefer-tcp "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"; RX_PID=
check "Linux sender uses the Wi-Fi alternative and token" \
    "[ $TX -eq 0 ] && cmp -s '$WORK/src/notes.txt' '$WORK/inbox5/notes.txt' && grep -q 'authorised by tap token' '$WORK/rx.log'"

echo "== Error handling"
"$CLI" send --tcp 127.0.0.1:9 /nonexistent/file.bin > "$WORK/tx.log" 2>&1; TX=$?
check "missing source file rejected before connecting" "[ $TX -ne 0 ] && grep -q 'file not found' '$WORK/tx.log'"
"$CLI" send --tcp 127.0.0.1:9 --connect-timeout 3 "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
check "connection refused -> gives up after timeout" "[ $TX -ne 0 ] && grep -q 'Connection failed permanently' '$WORK/tx.log'"
echo '{"transport":"tcp","port":1}' > "$WORK/bad.json"
"$CLI" send --handover-file "$WORK/bad.json" "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
check "malformed handover rejected" "[ $TX -ne 0 ] && grep -q 'Missing required field: ip' '$WORK/tx.log'"

echo "== Original demo executable (NetworkEngine API) against the ASP/1 receiver"
(cd "$WORK" && printf 'Hello from the AnyShare core engine. This is test file one.\n' > test1.txt \
    && ( "$CORE/receiver" 8080 "$WORK/demo-inbox" > rx.log 2>&1 & echo $! > rx.pid ) && sleep 0.5 \
    && timeout 20 "$CORE/demo" > demo.log 2>&1)
wait "$(cat "$WORK/rx.pid")" 2>/dev/null
check "demo delivered test1.txt intact" "cmp -s '$WORK/test1.txt' '$WORK/demo-inbox/test1.txt'"

echo "== Find receivers by name (UDP discovery on loopback)"
"$BUILD/discovery_tests" > "$WORK/discovery.log" 2>&1; D=$?
check "discovery tests (format, find, garbage, nothing there, port in use)" "[ $D -eq 0 ]"
[ $D -eq 0 ] || cat "$WORK/discovery.log"
"$CLI" receive --port 8094 --dir "$WORK/named-inbox" --yes --once --name "Test Laptop" \
    --discovery-port 48094 > "$WORK/rx.log" 2>&1 &
RX_PID=$!
sleep 0.5
"$CLI" discover --discovery-port 48094 > "$WORK/discover.log" 2>&1
check "anyshare discover lists 'Test Laptop'" "grep -q '^Test Laptop .*:8094' '$WORK/discover.log'"
"$CLI" send --to test --discovery-port 48094 "$WORK/src/notes.txt" > "$WORK/tx.log" 2>&1; TX=$?
wait "$RX_PID"
check "send --to NAME (prefix) delivers the file" "[ $TX -eq 0 ] && cmp -s '$WORK/src/notes.txt' '$WORK/named-inbox/notes.txt'"
check "sender labels Wi-Fi discovery (no NFC)" "grep -q \"Handover: Wi-Fi discovery: 'Test Laptop' (no NFC)\" '$WORK/tx.log'"

echo "== ESP32 OLED bridge protocol (a pseudo-terminal plays the ESP32)"
"$BUILD/bridge_tests" > "$WORK/bridge.log" 2>&1; B=$?
check "bridge tests (handshake, messages, unplug, throttling, real transfer)" "[ $B -eq 0 ]"
[ $B -eq 0 ] || cat "$WORK/bridge.log"

echo "== Desktop app (Qt, headless)"
if [ -x "$BUILD/anyshare-gui-tests" ]; then
    QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$WORK/xdg" timeout 120 "$BUILD/anyshare-gui-tests" > "$WORK/gui-tests.log" 2>&1; G=$?
    check "GUI unit tests (secret menu, no NFC text, send, demo tap, ESP32 display, accept/decline)" "[ $G -eq 0 ]"
    [ $G -eq 0 ] || grep -E "^FAIL|Loc:" "$WORK/gui-tests.log"
    # The real app as receiver, the CLI as sender; then Ctrl-C must close it cleanly.
    QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$WORK/xdg" "$BUILD/anyshare-gui" --receive --yes --no-nfc-probe \
        --esp32 off --port 8093 --dir "$WORK/gui-inbox" > "$WORK/gui-rx.log" 2>&1 &
    GUI_PID=$!
    sleep 1.5
    "$CLI" send --tcp 127.0.0.1:8093 "$WORK/src/photo.jpg" > "$WORK/tx.log" 2>&1; TX=$?
    sleep 0.5
    kill -INT "$GUI_PID"; wait "$GUI_PID"; GX=$?
    check "GUI received photo.jpg from the CLI" "[ $TX -eq 0 ] && cmp -s '$WORK/src/photo.jpg' '$WORK/gui-inbox/photo.jpg'"
    check "GUI logs the transfer in the terminal" "grep -q 'DONE: Received 1 file' '$WORK/gui-rx.log'"
    check "GUI exits cleanly on Ctrl-C" "[ $GX -eq 0 ]"
else
    echo "  (skipped: Qt Widgets dev package not installed)"
fi

echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
