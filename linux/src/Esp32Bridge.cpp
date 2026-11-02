#include "Esp32Bridge.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace anyshare::bridge {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// USB vendor id of the device behind a tty, or "" if it is not a USB device.
std::string usbVendorOf(const std::string& ttyName) {
    std::error_code ec;
    fs::path dir = fs::canonical("/sys/class/tty/" + ttyName + "/device", ec);
    for (int up = 0; !ec && up < 6 && !dir.empty(); ++up, dir = dir.parent_path()) {
        std::ifstream vendor(dir / "idVendor");
        std::string id;
        if (vendor >> id) return lower(id);
    }
    return "";
}

bool writeAll(int fd, const std::string& data, int timeoutMs) {
    std::size_t off = 0;
    while (off < data.size()) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

int openSerial(const std::string& port) {
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + port + ": " + std::strerror(errno));
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        throw std::runtime_error(port + " is not a serial port");
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(HUPCL | CRTSCTS);   // keep DTR/RTS on close: no ESP32 auto-reset
    tcsetattr(fd, TCSANOW, &tio);
    return fd;
}

// ESP32 dev boards wire RTS to EN (reset) and DTR to IO0 (boot mode) through two
// transistors. Both lines change in one ioctl so the board never sees a half state.
// Not a serial port with modem lines (e.g. a pty in tests): silently ignored.
void setResetLines(int fd, bool dtr, bool rts) {
    int bits = 0;
    if (ioctl(fd, TIOCMGET, &bits) != 0) return;
    bits = dtr ? (bits | TIOCM_DTR) : (bits & ~TIOCM_DTR);
    bits = rts ? (bits | TIOCM_RTS) : (bits & ~TIOCM_RTS);
    ioctl(fd, TIOCMSET, &bits);
}

// Normal boot (not flash mode): hold EN low with IO0 high, then release both.
void resetEsp32(int fd) {
    setResetLines(fd, false, true);
    usleep(120000);
    setResetLines(fd, false, false);
}

// Sends HELLO until the bridge's greeting arrives or the time is up.
// If the board stays silent (opening the port can leave an ESP32 in its flash
// mode on some systems), it is reset once into normal mode and asked again.
bool handshake(int fd, int timeoutMs) {
    setResetLines(fd, false, false);   // "run" state: EN high, IO0 high
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeoutMs);
    auto resetAt = start + std::chrono::milliseconds(std::min(1500, timeoutMs / 2));
    bool resetDone = timeoutMs < 2000;   // short probes (tests) never reset anything
    auto nextHello = std::chrono::steady_clock::now();
    std::string received;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!resetDone && std::chrono::steady_clock::now() >= resetAt) {
            resetDone = true;
            resetEsp32(fd);
            nextHello = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);   // boot banner comes first
        }
        if (std::chrono::steady_clock::now() >= nextHello) {
            writeAll(fd, "HELLO\n", 200);
            nextHello = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            char buf[256];
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) received.append(buf, static_cast<std::size_t>(n));
            if (received.find("AnyShare NFC bridge") != std::string::npos) return true;
            if (received.size() > 8192) received.erase(0, received.size() - 1024);
        }
    }
    return false;
}

}  // namespace

std::string fileTypeLabel(const std::string& fileName) {
    auto dot = fileName.rfind('.');
    std::string ext = dot == std::string::npos ? "" : lower(fileName.substr(dot + 1));
    struct Group {
        const char* label;
        std::vector<const char*> exts;
    };
    static const std::vector<Group> groups = {
        {"Image", {"jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "bmp", "svg"}},
        {"Video", {"mp4", "mkv", "mov", "avi", "webm", "3gp"}},
        {"Audio", {"mp3", "wav", "m4a", "aac", "flac", "ogg", "opus"}},
        {"PDF", {"pdf"}},
        {"Document", {"doc", "docx", "odt", "rtf"}},
        {"Spreadsheet", {"xls", "xlsx", "csv", "ods"}},
        {"Slides", {"ppt", "pptx", "odp"}},
        {"Text", {"txt", "md", "log", "json", "xml"}},
        {"Archive", {"zip", "rar", "7z", "tar", "gz"}},
        {"Android app", {"apk"}},
    };
    for (const auto& g : groups) {
        for (const char* e : g.exts) {
            if (ext == e) return g.label;
        }
    }
    return "File";
}

std::string displaySafe(const std::string& text, std::size_t maxChars) {
    std::string out;
    for (unsigned char c : text) {
        if (c >= 0x80 && c < 0xC0) continue;           // UTF-8 continuation byte
        if (c == '|') out += '/';
        else if (c < 0x20 || c > 0x7E) out += '?';      // control char or start of a UTF-8 sequence
        else out += static_cast<char>(c);
    }
    if (out.size() > maxChars && maxChars >= 12) {      // keep the end: it holds the extension
        std::size_t tail = 8;
        out = out.substr(0, maxChars - tail - 2) + ".." + out.substr(out.size() - tail);
    } else if (out.size() > maxChars) {
        out.resize(maxChars);
    }
    return out;
}

std::vector<std::string> candidatePorts() {
    static const std::vector<std::string> vendors = {"10c4", "1a86", "0403", "303a"};
    std::vector<std::string> ports;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/tty", ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("ttyUSB", 0) != 0 && name.rfind("ttyACM", 0) != 0) continue;
        std::string vendor = usbVendorOf(name);
        if (std::find(vendors.begin(), vendors.end(), vendor) != vendors.end()) ports.push_back("/dev/" + name);
    }
    std::sort(ports.begin(), ports.end());
    return ports;
}

std::unique_ptr<Esp32Bridge> Esp32Bridge::open(const std::string& port, int handshakeMs) {
    std::vector<std::string> ports = port == "auto" ? candidatePorts() : std::vector<std::string>{port};
    if (ports.empty()) throw std::runtime_error("no ESP32 USB serial port found (CP210x/CH34x/FTDI/Espressif)");
    std::string errors;
    for (const auto& p : ports) {
        try {
            int fd = openSerial(p);
            if (handshake(fd, handshakeMs)) {
                auto bridge = std::unique_ptr<Esp32Bridge>(new Esp32Bridge(fd, p));
                bridge->showIdle();
                return bridge;
            }
            ::close(fd);
            errors += (errors.empty() ? "" : "; ") + p + ": no AnyShare bridge answered";
        } catch (const std::exception& e) {
            errors += (errors.empty() ? "" : "; ") + std::string(e.what());
        }
    }
    throw std::runtime_error(errors);
}

Esp32Bridge::Esp32Bridge(int fd, std::string port) : fd_(fd), port_(std::move(port)) {}

Esp32Bridge::~Esp32Bridge() {
    setOnEvent(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        writeAll(fd_, "CARD|OFF\nIDLE\n", 200);   // never leave the module serving a stale address
        ::close(fd_);
    }
}

bool Esp32Bridge::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_ >= 0;
}

void Esp32Bridge::send(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return;
    if (!writeAll(fd_, line + "\n", 300)) {   // unplugged: stop trying, never disturb the transfer
        ::close(fd_);
        fd_ = -1;
    }
}

void Esp32Bridge::showTransfer(bool sending, int percent, const std::string& fileName, std::size_t index,
                               std::size_t count) {
    send(std::string("XFER|") + (sending ? "SEND" : "RECV") + "|" + std::to_string(std::clamp(percent, 0, 100)) + "|" +
         displaySafe(fileName) + "|" + fileTypeLabel(fileName) + "|" + std::to_string(index + 1) + "/" +
         std::to_string(std::max<std::size_t>(count, 1)));
}

void Esp32Bridge::showDone(bool ok, const std::string& summary) {
    send(std::string("DONE|") + (ok ? "OK" : "FAIL") + "|" + displaySafe(summary, 60));
}

void Esp32Bridge::showIdle() { send("IDLE"); }

void Esp32Bridge::serveHandover(const std::string& handoverJson) {
    std::string oneLine;
    for (char c : handoverJson) {
        if (c != '\n' && c != '\r') oneLine += c;
    }
    send("CARD|" + oneLine);
}

void Esp32Bridge::stopServing() { send("CARD|OFF"); }

void Esp32Bridge::writeToCard(const std::string& name, const std::string& data) {
    static const char* digits = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(data.size() * 2);
    for (unsigned char c : data) {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    send("WRITE|" + displaySafe(name, 40) + "|" + hex);
}

void Esp32Bridge::cancelCardWrite() { send("WRITECANCEL"); }

void Esp32Bridge::setOnEvent(std::function<void(const NfcEvent&)> onEvent) {
    stopReading_ = true;
    if (reader_.joinable()) reader_.join();
    stopReading_ = false;
    if (onEvent) reader_ = std::thread([this, onEvent] { readLoop(onEvent); });
}

void Esp32Bridge::readLoop(std::function<void(const NfcEvent&)> onEvent) {
    NfcLineParser parser;
    std::string pending;
    while (!stopReading_) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd = fd_;
        }
        if (fd < 0) return;   // unplugged
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        char buf[512];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) {   // the device went away
            std::lock_guard<std::mutex> lock(mutex_);
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
            return;
        }
        pending.append(buf, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (auto event = parser.feed(line)) onEvent(*event);
        }
        if (pending.size() > 4096) pending.clear();   // no newline in sight: noise
    }
}

// ---------------------------------------------------------------------------
std::string cardTypeFromSak(int sak) {
    switch (sak) {
        case 0x00: return "MIFARE Ultralight / NTAG";
        case 0x08: return "MIFARE Classic 1K";
        case 0x09: return "MIFARE Mini";
        case 0x18: return "MIFARE Classic 4K";
        case 0x10: case 0x11: return "MIFARE Plus";
        case 0x20: return "ISO 14443-4 card or phone";
        case 0x28: return "JCOP / dual-interface card";
        default: return sak < 0 ? "NFC tag" : "NFC tag (SAK 0x" + [&] {
            char b[3];
            std::snprintf(b, sizeof(b), "%02X", sak & 0xFF);
            return std::string(b);
        }() + ")";
    }
}

std::optional<NfcEvent> NfcLineParser::feed(const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    auto at = [&](const char* key) { return line.find(key); };

    if (auto pos = at("TAG uid="); pos != std::string::npos) {
        std::string rest = line.substr(pos + 8);
        lastUid_ = rest.substr(0, rest.find(' '));
        auto sakPos = rest.find("sak=0x");
        lastSak_ = sakPos == std::string::npos ? -1 : static_cast<int>(std::strtol(rest.c_str() + sakPos + 6, nullptr, 16));
        return std::nullopt;   // CARD / PAYLOAD / ERROR follows
    }
    if (auto pos = at("PAYLOAD "); pos != std::string::npos) {
        NfcEvent e;
        e.kind = NfcEvent::Kind::Phone;
        e.uid = lastUid_;
        e.sak = lastSak_;
        e.payload = line.substr(pos + 8);
        return e;
    }
    if (auto pos = at("CARD "); pos != std::string::npos) {
        (void)pos;
        NfcEvent e;
        e.kind = NfcEvent::Kind::Card;
        e.uid = lastUid_;
        e.sak = lastSak_;
        return e;
    }
    if (auto pos = at("CARDDATA "); pos != std::string::npos) {
        std::string rest = line.substr(pos + 9);
        auto bar = rest.find('|');
        if (bar == std::string::npos) return std::nullopt;
        NfcEvent e;
        e.kind = NfcEvent::Kind::CardFile;
        e.uid = lastUid_;
        e.message = rest.substr(0, bar);
        std::string hex = rest.substr(bar + 1);
        if (hex.size() % 2) return std::nullopt;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            e.data += static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        }
        return e;
    }
    if (auto pos = at("WRITTEN "); pos != std::string::npos) {
        NfcEvent e;
        e.kind = NfcEvent::Kind::Written;
        e.uid = lastUid_;
        std::string rest = line.substr(pos + 8);   // "<n> bytes <name>"
        auto name = rest.find("bytes ");
        e.message = name == std::string::npos ? rest : rest.substr(name + 6);
        return e;
    }
    if (auto pos = at("WRITEFAIL "); pos != std::string::npos) {
        NfcEvent e;
        e.kind = NfcEvent::Kind::WriteFailed;
        e.message = line.substr(pos + 10);
        return e;
    }
    if (auto pos = at("WPROG "); pos != std::string::npos) {
        NfcEvent e;
        e.kind = NfcEvent::Kind::WriteProgress;
        e.sak = std::atoi(line.c_str() + pos + 6);
        return e;
    }
    if (line.find("INFO AnyShare NFC bridge") != std::string::npos) {   // boot banner: state was lost
        NfcEvent e;
        e.kind = NfcEvent::Kind::Restarted;
        return e;
    }
    if (line.find("SERVED") != std::string::npos && line.find("CARDDBG") == std::string::npos) {
        NfcEvent e;
        e.kind = NfcEvent::Kind::Served;
        return e;
    }
    if (line.find("READER") == std::string::npos) {   // "READER ERROR" is wiring, not a tap
        if (auto pos = at("ERROR "); pos != std::string::npos) {
            NfcEvent e;
            e.kind = NfcEvent::Kind::Error;
            e.uid = lastUid_;
            e.message = line.substr(pos + 6);
            return e;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
void BridgeObserver::onSessionStarted(std::size_t fileCount, uint64_t) {
    fileCount_ = fileCount;   // the first screen waits for onFileStarted, which brings the name
    percent_ = 0;
}

void BridgeObserver::onFileStarted(std::size_t index, const std::string& name, uint64_t) {
    fileIndex_ = index;
    fileName_ = name;
    push(true);
}

void BridgeObserver::onBytesTransferred(uint64_t done, uint64_t total) {
    percent_ = total ? static_cast<int>(done * 100 / total) : 100;
    push(false);
}

void BridgeObserver::onSessionFinished(bool success, const std::string& message) {
    std::string summary;
    if (success) {
        summary = std::string(sending_ ? "Sent " : "Received ") + std::to_string(fileCount_) + " file(s)";
        if (!fileName_.empty()) summary += ": " + fileName_;
    } else {
        summary = message;
    }
    bridge_.showDone(success, summary);
}

void BridgeObserver::push(bool force) {
    auto now = std::chrono::steady_clock::now();
    // New percent at most every 150 ms; otherwise a keep-alive every 2 s so the
    // bridge's watchdog (15 s) does not give up on a slow link.
    bool changed = percent_ != lastSentPercent_ && now - lastSent_ >= std::chrono::milliseconds(150);
    bool keepAlive = now - lastSent_ >= std::chrono::seconds(2);
    if (!force && !changed && !keepAlive) return;
    lastSent_ = now;
    lastSentPercent_ = percent_;
    bridge_.showTransfer(sending_, percent_, fileName_, fileIndex_, fileCount_);
}

}  // namespace anyshare::bridge
