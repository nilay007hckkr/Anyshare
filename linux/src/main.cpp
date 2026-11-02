// anyshare -- Linux command-line sender/receiver for ASP/1.
//
// Transports: TCP (always), Bluetooth RFCOMM (ANYSHARE_HAVE_BLUEZ).
// Handover:   manual address, handover JSON file (simulated tap),
//             physical NFC via libnfc/PN532 (ANYSHARE_HAVE_LIBNFC).
// Every run prints which handover and which transport were *actually* used.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

#include <unistd.h>

#include "ConnectionInfoParser.hpp"
#include "Esp32Bridge.hpp"
#include "Handovers.hpp"
#include "JsonHandover.hpp"
#include "LanDiscovery.hpp"
#include "LinuxCommon.hpp"
#include "NetworkEngine.hpp"
#include "Protocol.hpp"
#include "SocketStream.hpp"
#include "TransferSession.hpp"

#ifdef ANYSHARE_HAVE_BLUEZ
#include "BluetoothRfcomm.hpp"
#endif
#ifdef ANYSHARE_HAVE_LIBNFC
#include "Pn532NfcHandover.hpp"
#endif

namespace fs = std::filesystem;
using namespace anyshare;
using namespace anyshare::host;

namespace {

std::atomic<bool> g_interrupted{false};

void onSignal(int) { g_interrupted.store(true); }

// Signal handlers may not take locks, so a watcher thread forwards Ctrl-C.
class InterruptWatcher {
public:
    explicit InterruptWatcher(std::function<void()> onInterrupt) : thread_([this, onInterrupt] {
        while (!stop_) {
            if (g_interrupted.exchange(false)) onInterrupt();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }) {}
    ~InterruptWatcher() {
        stop_ = true;
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

class ConsoleObserver : public IProgressObserver {
public:
    explicit ConsoleObserver(std::string verb) : verb_(std::move(verb)) {}
    void onProgress(int percent) override { (void)percent; }
    void onBytesTransferred(uint64_t done, uint64_t total) override {
        auto now = std::chrono::steady_clock::now();
        if (done != total && now - lastPrint_ < std::chrono::milliseconds(200)) return;
        lastPrint_ = now;
        double secs = std::chrono::duration<double>(now - start_).count();
        double rate = secs > 0 ? done / secs : 0;
        int percent = total ? static_cast<int>(done * 100 / total) : 100;
        std::printf("\r  %s %3d%%  %s / %s  (%s/s)   ", verb_.c_str(), percent, humanBytes(done).c_str(),
                    humanBytes(total).c_str(), humanBytes(static_cast<uint64_t>(rate)).c_str());
        std::fflush(stdout);
    }
    void onSessionStarted(std::size_t count, uint64_t total) override {
        start_ = std::chrono::steady_clock::now();
        std::printf("  %zu file(s), %s\n", count, humanBytes(total).c_str());
    }
    void onFileCompleted(std::size_t, const std::string& name, const std::string& path, bool verified) override {
        std::printf("\r  %s %s%s%s\n", verified ? "[verified]" : "[unverified]", name.c_str(),
                    path.empty() ? "" : " -> ", path.c_str());
    }
    void onError(const std::string& message) override { std::fprintf(stderr, "\n  ERROR: %s\n", message.c_str()); }
    void onSessionFinished(bool ok, const std::string& message) override {
        std::printf("\n%s: %s\n", ok ? "DONE" : "FAILED", message.c_str());
    }

private:
    std::string verb_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPrint_{};
};

// Optional ESP32 OLED display (linux/esp32-nfc-bridge). Never fatal.
std::unique_ptr<bridge::Esp32Bridge> openDisplay(const std::string& port) {
    if (port.empty() || port == "off") return nullptr;
    try {
        auto display = bridge::Esp32Bridge::open(port);
        std::printf("ESP32 display: %s\n", display->port().c_str());
        return display;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ESP32 display not used: %s\n", e.what());
        return nullptr;
    }
}

// Exact name first (case-insensitive), then a unique prefix.
std::optional<discovery::Peer> findPeerByName(const std::string& wanted, uint16_t discoveryPort) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::printf("Looking for '%s' on the network...\n", wanted.c_str());
    auto peers = discovery::discover(1500, discoveryPort, {"127.0.0.1"});
    std::vector<discovery::Peer> prefix;
    for (const auto& p : peers) {
        if (lower(p.name) == lower(wanted)) return p;
        if (lower(p.name).rfind(lower(wanted), 0) == 0) prefix.push_back(p);
    }
    if (prefix.size() == 1) return prefix.front();
    std::fprintf(stderr, "No single receiver named '%s' found.", wanted.c_str());
    if (peers.empty()) std::fprintf(stderr, " No receivers answered (are they in receive mode on this network?)\n");
    else {
        std::fprintf(stderr, " Receivers found:\n");
        for (const auto& p : peers) std::fprintf(stderr, "  %s (%s:%u)\n", p.name.c_str(), p.host.c_str(), p.port);
    }
    return std::nullopt;
}

int usage() {
    std::cerr <<
        "Usage:\n"
        "  anyshare receive [--transport tcp|rfcomm] [--dir DIR] [--port N] [--bind ADDR]\n"
        "                   [--advertise-ip ADDR] [--channel N] [--name NAME]\n"
        "                   [--handover-out FILE] [--yes] [--once] [--esp32 PORT|auto|off]\n"
        "                   [--no-discovery]   (default: phones and PCs find this receiver by name)\n"
        "  anyshare send <target> [--name NAME] [--connect-timeout SEC] [--esp32 PORT|auto|off] FILE...\n"
        "      targets: --to NAME (found on the network) | --tcp HOST:PORT | --bt ADDR[@CH]\n"
        "               | --handover-file FILE (simulated tap)\n"
        "               | --nfc [--nfc-device CONNSTRING] [--fallback-bt ADDR | --fallback-tcp HOST:PORT]\n"
        "      --bt-addr ADDR   address to use when a tap payload only names an Android device\n"
        "      --prefer-tcp     use the Wi-Fi address from a tap payload instead of Bluetooth\n"
        "      --esp32          show progress on the ESP32 bridge's OLED (auto = find it, off = do not)\n"
        "  anyshare discover [--timeout MS]      list AnyShare receivers on the network\n"
        "  anyshare nfc-probe [CONNSTRING]\n"
        "  anyshare bt-info\n"
        "Built with: TCP"
#ifdef ANYSHARE_HAVE_BLUEZ
        ", Bluetooth RFCOMM (BlueZ)"
#endif
#ifdef ANYSHARE_HAVE_LIBNFC
        ", NFC (libnfc)"
#endif
        "\n";
    return 2;
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------
int cmdReceive(const std::vector<std::string>& args) {
    std::string transport = "tcp", bind = "0.0.0.0", advertiseIp, name = hostName(), handoverOut, displayPort;
    const char* home = std::getenv("HOME");
    fs::path dir = home ? fs::path(home) / "Downloads" / "AnyShare" : fs::path("received");
    uint16_t port = 8080;
    int channel = 0;
    bool yes = false, once = false, discoverable = true;
    uint16_t discoveryPort = discovery::kPort;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + args[i]);
            return args[++i];
        };
        const std::string& a = args[i];
        if (a == "--transport") transport = next();
        else if (a == "--dir") dir = next();
        else if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--bind") bind = next();
        else if (a == "--advertise-ip") advertiseIp = next();
        else if (a == "--channel") channel = std::stoi(next());
        else if (a == "--name") name = next();
        else if (a == "--handover-out") handoverOut = next();
        else if (a == "--yes") yes = true;
        else if (a == "--once") once = true;
        else if (a == "--esp32") displayPort = next();
        else if (a == "--no-discovery") discoverable = false;
        else if (a == "--discovery-port") discoveryPort = static_cast<uint16_t>(std::stoi(next()));
        else throw std::invalid_argument("unknown option " + a);
    }
    (void)channel;   // only used with BlueZ
    auto display = openDisplay(displayPort);

    CancelToken cancel;
    InterruptWatcher watcher([&] { cancel.cancel(); });

    ConnectionInfo self;
    self.transport = transport;
    self.deviceName = name;
    self.sessionToken = protocol::randomHex(8);

    std::unique_ptr<TcpListener> tcp;
    std::unique_ptr<discovery::Responder> responder;
#ifdef ANYSHARE_HAVE_BLUEZ
    std::unique_ptr<bt::RfcommListener> rfcomm;
#endif
    if (transport == "tcp") {
        tcp = std::make_unique<TcpListener>(bind, port);
        self.ip = advertiseIp.empty() ? (bind == "0.0.0.0" ? firstLanIpv4() : bind) : advertiseIp;
        if (self.ip.empty()) self.ip = "127.0.0.1";
        self.port = tcp->port();
        std::printf("Listening: TCP %s:%u\n", bind.c_str(), tcp->port());
        if (discoverable) {
            responder = std::make_unique<discovery::Responder>(name, tcp->port(), "linux", discoveryPort);
            if (responder->running()) {
                std::printf("Discoverable as '%s' (UDP %u): senders pick this computer by name\n",
                            discovery::cleanName(name).c_str(), responder->udpPort());
            } else {
                std::printf("  Note: not discoverable by name (%s); senders need the address\n", responder->error().c_str());
            }
        }
    } else if (transport == "rfcomm") {
#ifdef ANYSHARE_HAVE_BLUEZ
        rfcomm = std::make_unique<bt::RfcommListener>(static_cast<uint8_t>(channel));
        self.btAddress = bt::localAdapterAddress();
        self.rfcommChannel = rfcomm->channel();
        std::printf("Listening: Bluetooth RFCOMM %s channel %u\n", self.btAddress.c_str(), rfcomm->channel());
        if (!rfcomm->sdpRegistered()) {
            std::printf("  WARNING: %s\n  Android senders need SDP; Linux senders can use --bt %s@%u\n",
                        rfcomm->sdpError().c_str(), self.btAddress.c_str(), rfcomm->channel());
        }
#else
        std::cerr << "This build has no Bluetooth support (install libbluetooth-dev and rebuild).\n";
        return 1;
#endif
    } else {
        throw std::invalid_argument("--transport must be tcp or rfcomm");
    }

    std::string payload = connectionInfoToJson(self);
    std::printf("Saving to: %s\n", dir.string().c_str());
    std::printf("Handover payload (what an NFC tap / QR code carries):\n  %s\n", payload.c_str());
    if (!handoverOut.empty()) {
        std::ofstream(handoverOut) << payload << "\n";
        std::printf("Wrote handover payload to %s (sender: anyshare send --handover-file %s ...)\n",
                    handoverOut.c_str(), handoverOut.c_str());
    }
    if (display) {
        display->serveHandover(payload);
        std::printf("ESP32 module: card mode on (phones can tap it to send here; experimental)\n");
    }
    std::printf("Offers carrying this session's token are accepted automatically; others %s.\n",
                yes ? "are accepted (--yes)" : "require confirmation");

    ReceiveOptions options;
    options.destinationDir = dir;
    options.expectedToken = self.sessionToken;
    options.acceptOffer = [&](const protocol::Offer& offer) {
        std::printf("\nIncoming offer from '%s': %zu file(s), %s\n", offer.senderName.c_str(), offer.files.size(),
                    humanBytes(offer.totalBytes()).c_str());
        for (std::size_t i = 0; i < offer.files.size() && i < 20; ++i) {
            std::printf("    %s (%s)\n", protocol::sanitizeFilename(offer.files[i].name).c_str(),
                        humanBytes(offer.files[i].size).c_str());
        }
        if (yes) return true;
        if (!isatty(STDIN_FILENO)) {
            std::printf("  Declined: no token and no terminal to confirm (use --yes for unattended tests).\n");
            return false;
        }
        std::printf("Accept? [y/N] ");
        std::fflush(stdout);
        std::string line;
        std::getline(std::cin, line);
        return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
    };

    int exitCode = 0;
    while (!cancel.isCancelled()) {
        std::unique_ptr<IByteStream> stream;
        if (tcp) stream = tcp->accept(-1, &cancel);
#ifdef ANYSHARE_HAVE_BLUEZ
        if (rfcomm) stream = rfcomm->accept(-1, &cancel);
#endif
        if (!stream) break;
        std::printf("\nConnection from %s\n", stream->describe().c_str());
        IByteStream* raw = stream.get();
        cancel.setOnCancel([raw] { raw->abort(); });
        ConsoleObserver observer("receiving");
        std::unique_ptr<bridge::BridgeObserver> shown;
        if (display) shown = std::make_unique<bridge::BridgeObserver>(*display, false);
        bridge::TeeObserver both(&observer, shown.get());
        ReceiveResult result = receiveFiles(*stream, options, &both, &cancel);
        cancel.setOnCancel(nullptr);
        std::printf("Transport: %s | sender authorised by %s\n", stream->describe().c_str(),
                    result.tokenMatched ? "tap token" : "manual confirmation");
        exitCode = result.status == TransferStatus::Success ? 0 : 1;
        if (once) break;
    }
    return exitCode;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------
std::unique_ptr<INfcHandover> manualTcp(const std::string& hostPort) {
    return std::make_unique<ManualHandover>(tcpTarget(hostPort), "manual address (no NFC)");
}

std::unique_ptr<INfcHandover> manualBt(const std::string& spec) {
    return std::make_unique<ManualHandover>(bluetoothTarget(spec), "Bluetooth fallback, manual address (no NFC)");
}

int cmdSend(const std::vector<std::string>& args) {
    std::unique_ptr<INfcHandover> handover;
    std::unique_ptr<INfcHandover> fallback;
    std::string name = hostName(), btOverride, displayPort, toName;
    uint16_t discoveryPort = discovery::kPort;
    bool useNfc = false, preferTcp = false;
    std::string nfcDevice;
    int connectTimeout = NetworkEngine::MAX_CONNECT_TIMEOUT_SECONDS;
    std::vector<std::string> files;

    for (std::size_t i = 0; i < args.size(); ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + args[i]);
            return args[++i];
        };
        const std::string& a = args[i];
        if (a == "--tcp") handover = manualTcp(next());
        else if (a == "--bt") handover = manualBt(next());
        else if (a == "--handover-file") {
            std::string path = next();
            std::ifstream in(path);
            if (!in) throw std::invalid_argument("cannot read " + path);
            std::stringstream ss;
            ss << in.rdbuf();
            handover = std::make_unique<JsonHandover>(ss.str(), "simulated tap: handover file " + path + " (no NFC hardware used)");
        } else if (a == "--nfc") useNfc = true;
        else if (a == "--nfc-device") nfcDevice = next();
        else if (a == "--fallback-bt") fallback = manualBt(next());
        else if (a == "--fallback-tcp") fallback = manualTcp(next());
        else if (a == "--bt-addr") btOverride = next();
        else if (a == "--prefer-tcp") preferTcp = true;
        else if (a == "--name") name = next();
        else if (a == "--connect-timeout") connectTimeout = std::stoi(next());
        else if (a == "--esp32") displayPort = next();
        else if (a == "--to") toName = next();
        else if (a == "--discovery-port") discoveryPort = static_cast<uint16_t>(std::stoi(next()));
        else if (!a.empty() && a[0] == '-') throw std::invalid_argument("unknown option " + a);
        else files.push_back(a);
    }

    if (useNfc) {
#ifdef ANYSHARE_HAVE_LIBNFC
        auto nfc = std::make_unique<Pn532NfcHandover>(nfcDevice);
        if (fallback) {
            // Fall back only when the hardware is missing, not when a tap
            // was read but its payload is bad.
            handover = std::make_unique<FallbackHandover>(std::move(nfc), std::move(fallback), [](const std::exception& e) {
                return dynamic_cast<const NfcUnavailableError*>(&e) != nullptr;
            });
        } else {
            handover = std::move(nfc);
        }
#else
        if (!fallback) {
            std::cerr << "This build has no NFC support (install libnfc-dev and rebuild), and no --fallback-* was given.\n";
            return 1;
        }
        std::cerr << "[Handover] NFC support not compiled in; using fallback: " << fallback->describe() << "\n";
        handover = std::move(fallback);
#endif
    }
    if (!toName.empty()) {
        auto peer = findPeerByName(toName, discoveryPort);
        if (!peer) return 1;
        handover = std::make_unique<ManualHandover>(tcpTarget(peer->host + ":" + std::to_string(peer->port)),
                                                    "Wi-Fi discovery: '" + peer->name + "' (no NFC)");
    }
    if (!handover || files.empty()) return usage();

    // Resolve the handover first so we can print exactly what happened.
    ConnectionInfo info;
    try {
        info = handover->performHandover();
    } catch (const std::exception& e) {
        std::cerr << "Handover failed (" << handover->describe() << "): " << e.what() << "\n";
        return 1;
    }
    if (!btOverride.empty()) info.btAddress = btOverride;
    bool tcpAlternative = hasWifiAlternative(info);
#ifndef ANYSHARE_HAVE_BLUEZ
    if (tcpAlternative && !preferTcp) {
        std::printf("Note: this build has no Bluetooth; using the receiver's Wi-Fi address instead.\n");
        preferTcp = true;
    }
#endif
    if (tcpAlternative && preferTcp) info.transport = "tcp";
    info.files.clear();
    for (const auto& f : files) info.files.push_back({f, "", 0, ""});

    std::printf("Handover: %s\n", handover->describe().c_str());
    std::printf("Target:   %s\n", info.transport == "tcp"
                                      ? (info.ip + ":" + std::to_string(info.port)).c_str()
                                      : ("Bluetooth " + (info.btAddress.empty() ? "'" + info.deviceName + "'" : info.btAddress)).c_str());

    NetworkEngine engine;
    engine.setSenderName(name);
    engine.setConnectTimeoutSeconds(connectTimeout);
#ifdef ANYSHARE_HAVE_BLUEZ
    engine.setStreamFactory(bt::makeStreamFactory());
#endif
    ConsoleObserver observer("sending");
    engine.addObserver(&observer);
    auto display = openDisplay(displayPort);
    std::unique_ptr<bridge::BridgeObserver> shown;
    if (display) {
        shown = std::make_unique<bridge::BridgeObserver>(*display, true);
        engine.addObserver(shown.get());
    }
    InterruptWatcher watcher([&] { engine.cancel(); });
    engine.connectToHost(info);
    if (!engine.lastTransport().empty()) std::printf("Transport: %s\n", engine.lastTransport().c_str());
    return engine.lastTransferSucceeded() ? 0 : 1;
}

int cmdDiscover(const std::vector<std::string>& args) {
    int timeoutMs = 1500;
    uint16_t port = discovery::kPort;
    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        if (args[i] == "--timeout") timeoutMs = std::stoi(args[i + 1]);
        else if (args[i] == "--discovery-port") port = static_cast<uint16_t>(std::stoi(args[i + 1]));
        else throw std::invalid_argument("unknown option " + args[i]);
    }
    auto peers = discovery::discover(timeoutMs, port, {"127.0.0.1"});
    if (peers.empty()) {
        std::printf("No AnyShare receivers found (they must be in receive mode on this network).\n");
        return 1;
    }
    for (const auto& p : peers) {
        std::printf("%-32s %s:%u  (%s)\n", p.name.c_str(), p.host.c_str(), p.port, p.os.c_str());
    }
    return 0;
}

int cmdNfcProbe(const std::vector<std::string>& args) {
#ifdef ANYSHARE_HAVE_LIBNFC
    try {
        std::cout << Pn532NfcHandover::probe(args.empty() ? "" : args[0]);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NFC hardware NOT available: " << e.what() << "\n";
        return 1;
    }
#else
    (void)args;
    std::cerr << "NFC support not compiled in (install libnfc-dev and rebuild).\n";
    return 1;
#endif
}

int cmdBtInfo() {
#ifdef ANYSHARE_HAVE_BLUEZ
    try {
        std::string address = bt::localAdapterAddress();   // throws before printing anything
        std::cout << "Bluetooth adapter: " << address << "\n"
                  << "AnyShare service UUID: " << protocol::kBluetoothServiceUuid << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Bluetooth NOT available: " << e.what() << "\n";
        return 1;
    }
#else
    std::cerr << "Bluetooth support not compiled in (install libbluetooth-dev and rebuild).\n";
    return 1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    try {
        if (cmd == "receive") return cmdReceive(args);
        if (cmd == "send") return cmdSend(args);
        if (cmd == "discover") return cmdDiscover(args);
        if (cmd == "nfc-probe") return cmdNfcProbe(args);
        if (cmd == "bt-info") return cmdBtInfo();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    return usage();
}
