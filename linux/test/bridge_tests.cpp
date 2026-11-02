// Tests for the ESP32 bridge link. A pseudo-terminal plays the ESP32, so no
// hardware is needed: ./build-linux/bridge_tests
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Esp32Bridge.hpp"
#include "FakeEsp32.hpp"
#include "SocketStream.hpp"
#include "TransferSession.hpp"

#include <mutex>

using namespace anyshare;
using namespace anyshare::bridge;
namespace fs = std::filesystem;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                               \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                         \
    } while (0)

void testLabels() {
    CHECK(fileTypeLabel("IMG_2024.JPG") == "Image");
    CHECK(fileTypeLabel("clip.mp4") == "Video");
    CHECK(fileTypeLabel("notes.pdf") == "PDF");
    CHECK(fileTypeLabel("app-release.apk") == "Android app");
    CHECK(fileTypeLabel("Makefile") == "File");
    CHECK(fileTypeLabel("archive.tar.gz") == "Archive");

    CHECK(displaySafe("caf\xC3\xA9.png") == "caf?.png");                 // UTF-8 -> one '?'
    CHECK(displaySafe("a|b\tc") == "a/b?c");                             // separator and control chars
    std::string longName = "a_really_long_holiday_photo_name_from_goa_2026.jpeg";
    std::string shown = displaySafe(longName, 21);
    CHECK(shown.size() == 21);
    CHECK(shown.substr(shown.size() - 5) == ".jpeg");                    // extension survives truncation
}

void testHandshakeAndMessages() {
    FakeEsp32 esp(true);
    auto bridge = Esp32Bridge::open(esp.path(), 2000);
    CHECK(bridge && bridge->connected());
    CHECK(bridge->port() == esp.path());
    auto lines = esp.waitFor(2);
    CHECK(!lines.empty() && lines.front() == "HELLO");
    CHECK(!lines.empty() && lines.back() == "IDLE");   // open() resets the screen

    esp.clear();
    bridge->writeToCard("note|1.txt", std::string("Hi\x00\xff", 4));
    lines = esp.waitFor(1);
    CHECK(!lines.empty() && lines.back() == "WRITE|note/1.txt|486900FF");
    esp.clear();
    bridge->serveHandover("{\"ip\":\"10.0.0.2\",\n\"port\":8080}");
    bridge->stopServing();
    lines = esp.waitFor(2);
    CHECK(lines.size() == 2 && lines[0] == "CARD|{\"ip\":\"10.0.0.2\",\"port\":8080}" && lines[1] == "CARD|OFF");
    esp.clear();
    bridge->showTransfer(false, 42, "photo.jpg", 0, 3);
    bridge->showTransfer(true, 140, "x|y.pdf", 2, 3);
    bridge->showDone(false, "connection lost");
    lines = esp.waitFor(3);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == "XFER|RECV|42|photo.jpg|Image|1/3");
        CHECK(lines[1] == "XFER|SEND|100|x/y.pdf|PDF|3/3");   // clamped, '|' escaped
        CHECK(lines[2] == "DONE|FAIL|connection lost");
    }

    // Unplugged: sending must neither throw nor block, and the bridge goes quiet.
    esp.unplug();
    for (int i = 0; i < 3; ++i) bridge->showIdle();
    CHECK(!bridge->connected());
}

void testSilentPortIsRejected() {
    FakeEsp32 other(false);   // some other serial device: never answers HELLO
    auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        Esp32Bridge::open(other.path(), 500);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("no AnyShare bridge answered") != std::string::npos;
    }
    CHECK(threw);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    bool missing = false;
    try {
        Esp32Bridge::open("/dev/does-not-exist", 200);
    } catch (const std::exception&) {
        missing = true;
    }
    CHECK(missing);
}

void testObserverThrottling() {
    FakeEsp32 esp(true);
    auto bridge = Esp32Bridge::open(esp.path(), 2000);
    esp.waitFor(2);
    esp.clear();
    BridgeObserver obs(*bridge, true);
    obs.onSessionStarted(2, 2000);
    obs.onFileStarted(0, "clip.mp4", 1000);
    for (uint64_t done = 1; done <= 1000; ++done) obs.onBytesTransferred(done, 2000);   // a burst: throttled
    obs.onSessionFinished(true, "ok");
    auto lines = esp.waitFor(3);
    CHECK(lines.size() >= 2 && lines.size() <= 4);
    CHECK(!lines.empty() && lines[0] == "XFER|SEND|0|clip.mp4|Video|1/2");   // never a screen without a name
    CHECK(!lines.empty() && lines.back() == "DONE|OK|Sent 2 file(s): clip.mp4");
}

// A real loopback transfer: both sides drive their own display.
void testRealTransferDrivesDisplays() {
    FakeEsp32 rxEsp(true), txEsp(true);
    auto rxBridge = Esp32Bridge::open(rxEsp.path(), 2000);
    auto txBridge = Esp32Bridge::open(txEsp.path(), 2000);
    rxEsp.waitFor(2);
    txEsp.waitFor(2);
    rxEsp.clear();
    txEsp.clear();

    fs::path dir = fs::temp_directory_path() / ("bridge_tests_" + std::to_string(getpid()));
    fs::create_directories(dir / "in");
    {
        std::ofstream f(dir / "song.mp3", std::ios::binary);
        std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 64; ++i) f << chunk;   // 4 MiB
    }
    TcpListener listener("127.0.0.1", 0);
    ReceiveResult received;
    std::thread rx([&] {
        auto stream = listener.accept(5000);
        ReceiveOptions options;
        options.destinationDir = dir / "in";
        options.acceptOffer = [](const protocol::Offer&) { return true; };
        BridgeObserver obs(*rxBridge, false);
        if (stream) received = receiveFiles(*stream, options, &obs);
    });
    auto stream = connectTcp("127.0.0.1", listener.port(), 5000);
    BridgeObserver txObs(*txBridge, true);
    SendOptions sendOptions;
    sendOptions.senderName = "bridge-test";
    SendResult sent = sendFiles(*stream, {outgoingFileFromPath((dir / "song.mp3").string())}, sendOptions, &txObs);
    rx.join();
    CHECK(sent.status == TransferStatus::Success);
    CHECK(received.status == TransferStatus::Success);

    auto rxLines = rxEsp.waitFor(3), txLines = txEsp.waitFor(3);
    bool rxSawFile = false, txSawFile = false;
    for (const auto& l : rxLines) rxSawFile |= l.rfind("XFER|RECV|", 0) == 0 && l.find("|song.mp3|Audio|1/1") != std::string::npos;
    for (const auto& l : txLines) txSawFile |= l.rfind("XFER|SEND|", 0) == 0 && l.find("|song.mp3|Audio|1/1") != std::string::npos;
    CHECK(rxSawFile);
    CHECK(txSawFile);
    CHECK(!rxLines.empty() && rxLines.back() == "DONE|OK|Received 1 file(s): song.mp3");
    CHECK(!txLines.empty() && txLines.back() == "DONE|OK|Sent 1 file(s): song.mp3");
    fs::remove_all(dir);
}

void testParser() {
    NfcLineParser parser;
    CHECK(!parser.feed("TAG uid=7672F906 sak=0x08\r"));
    auto card = parser.feed("CARD plain NFC card/tag, not a phone (the reader works)");
    CHECK(card && card->kind == NfcEvent::Kind::Card && card->uid == "7672F906" && card->sak == 0x08);
    CHECK(cardTypeFromSak(0x08) == "MIFARE Classic 1K");
    CHECK(cardTypeFromSak(0x42) == "NFC tag (SAK 0x42)");

    CHECK(!parser.feed("TAG uid=08A1B2C3 sak=0x20"));
    auto phone = parser.feed("PAYLOAD {\"ip\":\"10.0.0.7\",\"port\":8080}");
    CHECK(phone && phone->kind == NfcEvent::Kind::Phone && phone->payload == "{\"ip\":\"10.0.0.7\",\"port\":8080}");

    auto noisy = parser.feed("\xFF\xFF\xFF" "CARD plain NFC card/tag");   // buzzer noise before the keyword
    CHECK(noisy && noisy->kind == NfcEvent::Kind::Card);
    auto failed = parser.feed("ERROR phone answered SW 6A82: open AnyShare and tap Receive first");
    CHECK(failed && failed->kind == NfcEvent::Kind::Error && failed->message.rfind("phone answered SW 6A82", 0) == 0);
    CHECK(!parser.feed("READER ERROR no answer over SPI"));   // wiring problem, not a tap
    CHECK(!parser.feed("SCREEN Ready | Tap an NFC card"));
    CHECK(!parser.feed("LED green solid"));
    auto served = parser.feed("SERVED handover read by a phone");
    CHECK(served && served->kind == NfcEvent::Kind::Served);
    CHECK(!parser.feed("CARDMODE on (99-byte handover, Config ok)"));
    auto file = parser.feed("CARDDATA hello.txt|48656C6C6F");
    CHECK(file && file->kind == NfcEvent::Kind::CardFile && file->message == "hello.txt" && file->data == "Hello");
    auto written = parser.feed("WRITTEN 78 bytes hello.txt");
    CHECK(written && written->kind == NfcEvent::Kind::Written && written->message == "hello.txt");
    auto locked = parser.feed("WRITEFAIL card is locked (sector 1 does not use the default key)");
    CHECK(locked && locked->kind == NfcEvent::Kind::WriteFailed && locked->message.rfind("card is locked", 0) == 0);
    auto progress = parser.feed("WPROG 40");
    CHECK(progress && progress->kind == NfcEvent::Kind::WriteProgress && progress->sak == 40);
    CHECK(!parser.feed("WRITEWAIT 78 bytes hello.txt: tap a MIFARE Classic 1K card"));
    auto boot = parser.feed("INFO AnyShare NFC bridge (ESP32 + Si512) v1");
    CHECK(boot && boot->kind == NfcEvent::Kind::Restarted);
}

void testEventsFromDevice() {
    FakeEsp32 esp(true);
    auto bridge = Esp32Bridge::open(esp.path(), 2000);
    std::mutex m;
    std::vector<NfcEvent> events;
    bridge->setOnEvent([&](const NfcEvent& e) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(e);
    });
    esp.emitLine("TAG uid=7672F906 sak=0x08");
    esp.emitLine("CARD plain NFC card/tag, not a phone (the reader works)");
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (!events.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(events.size() == 1 && events[0].uid == "7672F906");
    }
    bridge->setOnEvent(nullptr);   // returns only once the reader thread has stopped
    bridge->showIdle();            // writing still works afterwards
    CHECK(bridge->connected());
}

int main() {
    testParser();
    testEventsFromDevice();
    testLabels();
    testHandshakeAndMessages();
    testSilentPortIsRejected();
    testObserverThrottling();
    testRealTransferDrivesDisplays();
    std::printf("bridge_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
