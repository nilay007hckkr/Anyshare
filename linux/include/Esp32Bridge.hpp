#pragma once

// Link to the ESP32 NFC bridge (linux/esp32-nfc-bridge) over its USB serial
// port. The laptop tells the ESP32's OLED what is happening; one line per
// message, fields separated by '|':
//
//   HELLO                                      -> bridge answers "INFO AnyShare NFC bridge ..."
//   XFER|SEND or RECV|percent|file|type|i/n    transfer in progress (file i of n)
//   DONE|OK or FAIL|summary                    shown briefly, then the idle screen
//   IDLE                                       back to "tap an NFC card or phone"
//   CARD|{json} / CARD|OFF                     card mode on/off: the module then answers phones
//                                              like an NFC card with this laptop's handover
//
// and reads what the reader saw (firmware -> laptop):
//   TAG uid=7672F906 sak=0x08   then  CARD ...        a plain card / tag
//   TAG uid=... sak=0x20        then  PAYLOAD {json}  an AnyShare phone in receive mode (real NFC handover)
//   ERROR <reason>                                    a tap that failed
//   SERVED ...                                        card mode: a phone read our handover
//   WPROG n / WRITTEN n bytes name / WRITEFAIL why    writing a file onto a MIFARE Classic card
//   CARDDATA name|hex                                 a tapped card holds a file (card -> laptop)
//
// Display updates are best effort: a missing or unplugged bridge never
// affects a transfer.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "IProgressObserver.hpp"

namespace anyshare::bridge {

// "Image", "Video", "PDF", ... from the file extension (shown on the OLED).
std::string fileTypeLabel(const std::string& fileName);

// Printable ASCII only (the OLED font has no UTF-8), '|' replaced, at most maxChars.
std::string displaySafe(const std::string& text, std::size_t maxChars = 40);

// Serial ports whose USB vendor is a common ESP32 USB-UART chip (CP210x, CH34x,
// FTDI, Espressif native USB). Other serial devices are never touched.
std::vector<std::string> candidatePorts();

// Something the reader saw.
struct NfcEvent {
    enum class Kind { Card, Phone, Error, Served, Restarted, CardFile, Written, WriteFailed, WriteProgress } kind = Kind::Card;
    // Restarted: firmware booted again. CardFile: name + data read from a card. Written / WriteFailed /
    // WriteProgress: result of writeToCard() (percent in `sak` for WriteProgress, reason in `message`).
    std::string uid;        // hex, e.g. "7672F906" (Card, Phone)
    int sak = -1;           // ISO 14443A SAK, -1 if unknown
    std::string payload;    // Phone: the receiver's handover JSON
    std::string message;    // Error / WriteFailed: why; CardFile / Written: file name
    std::string data;       // CardFile: the file's bytes
};

// "MIFARE Classic 1K", "MIFARE Ultralight / NTAG", ... from the SAK byte.
std::string cardTypeFromSak(int sak);

// Turns the firmware's serial lines into events. Tolerates noise around the
// keywords (the buzzer can garble a few bytes) and ignores everything else.
class NfcLineParser {
public:
    std::optional<NfcEvent> feed(const std::string& line);

private:
    std::string lastUid_;
    int lastSak_ = -1;
};

class Esp32Bridge {
public:
    // Opens `port` ("auto" = probe candidatePorts()) and checks the bridge's
    // greeting. Throws std::runtime_error if no bridge answers.
    // A silent board is reset once into normal mode (some systems leave it in flash mode on open).
    static std::unique_ptr<Esp32Bridge> open(const std::string& port, int handshakeMs = 4500);

    ~Esp32Bridge();
    Esp32Bridge(const Esp32Bridge&) = delete;
    Esp32Bridge& operator=(const Esp32Bridge&) = delete;

    const std::string& port() const { return port_; }
    bool connected() const;

    // Thread-safe, never throws. After a write error the bridge stays silent.
    void send(const std::string& line);

    void showTransfer(bool sending, int percent, const std::string& fileName, std::size_t index, std::size_t count);
    void showDone(bool ok, const std::string& summary);
    void showIdle();

    // Card mode (experimental): while this laptop receives, the module acts as
    // an NFC card that hands out `handoverJson`, so a phone can tap it to send here.
    void serveHandover(const std::string& handoverJson);
    void stopServing();

    // Demo without an NFC phone: the next MIFARE Classic 1K card tapped stores this
    // file (about 700 bytes at most, default key FFFFFFFFFFFF); tapping it later reads it back.
    static constexpr std::size_t kCardCapacity = 45 * 16 - 9;   // minus header; name counts too
    void writeToCard(const std::string& name, const std::string& data);
    void cancelCardWrite();

    // Reads the reader's events on a background thread and calls `onEvent`
    // there (callers forward it to their UI thread). nullptr stops reading;
    // it returns only after the thread has stopped.
    void setOnEvent(std::function<void(const NfcEvent&)> onEvent);

private:
    Esp32Bridge(int fd, std::string port);
    void readLoop(std::function<void(const NfcEvent&)> onEvent);

    int fd_;
    std::string port_;
    mutable std::mutex mutex_;
    std::thread reader_;
    std::atomic<bool> stopReading_{false};
};

// Forwards transfer progress to the bridge (at most ~6 updates/s, plus every
// file change and the final result).
class BridgeObserver : public IProgressObserver {
public:
    BridgeObserver(Esp32Bridge& bridge, bool sending) : bridge_(bridge), sending_(sending) {}

    void onProgress(int) override {}
    void onSessionStarted(std::size_t fileCount, uint64_t totalBytes) override;
    void onFileStarted(std::size_t index, const std::string& name, uint64_t size) override;
    void onBytesTransferred(uint64_t done, uint64_t total) override;
    void onSessionFinished(bool success, const std::string& message) override;

private:
    void push(bool force);

    Esp32Bridge& bridge_;
    bool sending_;
    std::size_t fileCount_ = 0, fileIndex_ = 0;
    std::string fileName_;
    int percent_ = 0, lastSentPercent_ = -1;
    std::chrono::steady_clock::time_point lastSent_{};
};

// Sends every callback to two observers (e.g. the console and the bridge).
class TeeObserver : public IProgressObserver {
public:
    TeeObserver(IProgressObserver* a, IProgressObserver* b) : a_(a), b_(b) {}
    void onProgress(int p) override { each([&](IProgressObserver* o) { o->onProgress(p); }); }
    void onError(const std::string& m) override { each([&](IProgressObserver* o) { o->onError(m); }); }
    void onSessionStarted(std::size_t n, uint64_t t) override { each([&](IProgressObserver* o) { o->onSessionStarted(n, t); }); }
    void onFileStarted(std::size_t i, const std::string& n, uint64_t s) override {
        each([&](IProgressObserver* o) { o->onFileStarted(i, n, s); });
    }
    void onBytesTransferred(uint64_t d, uint64_t t) override { each([&](IProgressObserver* o) { o->onBytesTransferred(d, t); }); }
    void onFileCompleted(std::size_t i, const std::string& n, const std::string& p, bool v) override {
        each([&](IProgressObserver* o) { o->onFileCompleted(i, n, p, v); });
    }
    void onSessionFinished(bool ok, const std::string& m) override { each([&](IProgressObserver* o) { o->onSessionFinished(ok, m); }); }

private:
    template <typename F>
    void each(F f) {
        if (a_) f(a_);
        if (b_) f(b_);
    }
    IProgressObserver* a_;
    IProgressObserver* b_;
};

}  // namespace anyshare::bridge
