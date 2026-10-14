// Self-contained unit + integration tests for the ASP/1 core (no test framework
// dependency). Integration tests run real TCP sessions over 127.0.0.1.
//
//   ./core_tests                 run everything (large-file test uses 256 MiB)
//   ANYSHARE_LARGE_MB=2048 ./core_tests   larger stress file
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

#include "Checksum.hpp"
#include "ConnectionInfoParser.hpp"
#include "NetworkEngine.hpp"
#include "Protocol.hpp"
#include "SocketStream.hpp"
#include "TransferSession.hpp"

namespace fs = std::filesystem;
using namespace anyshare;
using namespace anyshare::protocol;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond << "\n"; \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        auto va = (a);                                                                              \
        auto vb = (b);                                                                              \
        if (!(va == vb)) {                                                                          \
            ++g_failures;                                                                           \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b << "  ('" \
                      << va << "' vs '" << vb << "')\n";                                            \
        }                                                                                           \
    } while (0)

template <typename F>
static bool throwsWith(F f, const std::string& fragment) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(fragment) != std::string::npos) return true;
        std::cerr << "    (threw '" << e.what() << "', expected '" << fragment << "')\n";
        return false;
    }
    return false;
}

// --- helpers ----------------------------------------------------------------

struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / ("anyshare-test-" + randomHex(6))) { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static void writeRandomFile(const fs::path& p, uint64_t size, uint64_t seed) {
    std::ofstream out(p, std::ios::binary);
    std::vector<char> buf(1 << 20);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    while (size > 0) {
        std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(size, buf.size()));
        for (std::size_t i = 0; i < n; i += 8) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            std::memcpy(buf.data() + i, &x, std::min<std::size_t>(8, n - i));
        }
        out.write(buf.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
}

static std::string fileCrc(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> buf(1 << 20);
    Crc32Calculator crc;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        crc.update(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    return crc.getHexValue();
}

static std::size_t countEntries(const fs::path& dir) {
    std::size_t n = 0;
    for (auto& e : fs::directory_iterator(dir)) { (void)e; ++n; }
    return n;
}

// Wraps a stream to inject faults on the write path.
class FaultyStream : public IByteStream {
public:
    FaultyStream(std::unique_ptr<IByteStream> inner, uint64_t dropAfter, uint64_t corruptAt = UINT64_MAX)
        : inner_(std::move(inner)), dropAfter_(dropAfter), corruptAt_(corruptAt) {}
    std::size_t readSome(void* b, std::size_t n) override { return inner_->readSome(b, n); }
    void writeAll(const void* b, std::size_t n) override {
        std::vector<char> copy(static_cast<const char*>(b), static_cast<const char*>(b) + n);
        if (corruptAt_ >= written_ && corruptAt_ < written_ + n) copy[corruptAt_ - written_] ^= 0x5A;
        if (written_ + n > dropAfter_) {
            std::size_t part = static_cast<std::size_t>(dropAfter_ - written_);
            if (part) inner_->writeAll(copy.data(), part);
            written_ += part;
            inner_->abort();
            throw TransportError("injected link drop");
        }
        inner_->writeAll(copy.data(), n);
        written_ += n;
    }
    void abort() override { inner_->abort(); }
    std::string describe() const override { return "faulty(" + inner_->describe() + ")"; }

private:
    std::unique_ptr<IByteStream> inner_;
    uint64_t dropAfter_;
    uint64_t corruptAt_;
    uint64_t written_ = 0;
};

// Receiver running on a background thread for `sessions` connections.
struct ReceiverHarness {
    TcpListener listener{"127.0.0.1", 0};
    ReceiveOptions options;
    std::vector<ReceiveResult> results;
    std::thread thread;

    explicit ReceiverHarness(const fs::path& dir) {
        options.destinationDir = dir;
        options.reserveBytes = 0;
        options.acceptOffer = [](const Offer&) { return true; };
    }
    void start(int sessions = 1) {
        thread = std::thread([this, sessions] {
            for (int i = 0; i < sessions; ++i) {
                auto s = listener.accept(15000);
                if (!s) break;
                results.push_back(receiveFiles(*s, options));
            }
        });
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
    ~ReceiverHarness() { join(); }
};

static SendResult sendOver(uint16_t port, const std::vector<OutgoingFile>& files, SendOptions opts = {},
                           IProgressObserver* obs = nullptr, CancelToken* cancel = nullptr) {
    auto s = connectTcp("127.0.0.1", port, 5000);
    return sendFiles(*s, files, opts, obs, cancel);
}

// --- unit tests ---------------------------------------------------------------

static void testCrc32() {
    Crc32Calculator a;
    a.update("123456789", 9);
    CHECK_EQ(a.getHexValue(), std::string("cbf43926"));   // standard check value
    CHECK_EQ(Crc32Calculator().getHexValue(), std::string("00000000"));
    // Slicing-by-8 must equal byte-at-a-time for every alignment/length.
    std::string data(1000, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 131 + 7);
    for (std::size_t len : {1u, 7u, 8u, 9u, 63u, 999u}) {
        Crc32Calculator whole, bytes;
        whole.update(data.data(), len);
        for (std::size_t i = 0; i < len; ++i) bytes.update(data.data() + i, 1);
        CHECK_EQ(whole.value(), bytes.value());
    }
}

static void testSanitize() {
    CHECK_EQ(sanitizeFilename("../../etc/passwd"), std::string("passwd"));
    CHECK_EQ(sanitizeFilename("..\\..\\boot.ini"), std::string("boot.ini"));
    CHECK_EQ(sanitizeFilename(".."), std::string("file"));
    CHECK_EQ(sanitizeFilename("."), std::string("file"));
    CHECK_EQ(sanitizeFilename(""), std::string("file"));
    CHECK_EQ(sanitizeFilename("dir/"), std::string("file"));
    CHECK_EQ(sanitizeFilename(".bashrc"), std::string("_bashrc"));
    CHECK_EQ(sanitizeFilename(std::string("a\x01" "b\x7f.txt")), std::string("a_b_.txt"));
    CHECK_EQ(sanitizeFilename("what?.txt"), std::string("what_.txt"));
    CHECK_EQ(sanitizeFilename("CON.txt"), std::string("_CON.txt"));
    CHECK_EQ(sanitizeFilename("\xff\xfe.txt"), std::string("__.txt"));
    CHECK_EQ(sanitizeFilename("h\xc3\xa9llo \xe2\x9c\x93.pdf"), std::string("h\xc3\xa9llo \xe2\x9c\x93.pdf"));
    CHECK_EQ(sanitizeFilename("  report.pdf.  "), std::string("report.pdf"));
    std::string longName = std::string(300, 'x') + ".txt";
    std::string s = sanitizeFilename(longName);
    CHECK(s.size() <= 200);
    CHECK(s.size() > 4 && s.substr(s.size() - 4) == ".txt");
    // Multi-byte characters must not be cut in half.
    std::string utf;
    for (int i = 0; i < 120; ++i) utf += "\xc3\xa9";
    std::string t = sanitizeFilename(utf);
    CHECK(t.size() <= 200 && t.size() % 2 == 0);
}

static void testHandoverParser() {
    auto mainSchema = parseConnectionInfo(R"({"ssid":"DIRECT-xy","psk":"pw","ip":"192.168.49.1","port":8080,
        "files":[{"filename":"a.mp4","mimeType":"video/mp4","filesize":10,"checksum":"a1b2c3d4"}]})");
    CHECK_EQ(mainSchema.transport, std::string("tcp"));
    CHECK_EQ(mainSchema.port, 8080);
    CHECK_EQ(mainSchema.files.size(), 1u);
    CHECK_EQ(mainSchema.files[0].checksum, std::string("a1b2c3d4"));

    auto legacy = parseConnectionInfo(R"({"ssid":"s","psk":"p","ip":"192.168.49.1","port":8080,
        "filename":"x.bin","filesize":42})");
    CHECK_EQ(legacy.files.size(), 1u);
    CHECK_EQ(legacy.files[0].filesize, 42u);

    auto bt = parseConnectionInfo(R"({"v":1,"transport":"rfcomm","name":"Pixel 7","token":"abcd"})");
    CHECK_EQ(bt.transport, std::string("rfcomm"));
    CHECK_EQ(bt.deviceName, std::string("Pixel 7"));
    // Android receiver payload: BT name + Wi-Fi alternative.
    auto android = parseConnectionInfo(R"({"v":1,"transport":"rfcomm","name":"Pixel 7","ip":"192.168.1.9","port":8080,"token":"ab"})");
    CHECK_EQ(android.ip, std::string("192.168.1.9"));
    CHECK_EQ(android.port, 8080);
    auto inferred = parseConnectionInfo(R"({"bt":"AA:BB:CC:DD:EE:FF","ch":3})");
    CHECK_EQ(inferred.transport, std::string("rfcomm"));
    CHECK_EQ(int(inferred.rfcommChannel), 3);

    CHECK(throwsWith([] { parseConnectionInfo(R"({"port":8080})"); }, "Missing required field: ip"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"ip":"1.2.3.4"})"); }, "Missing required field: port"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"ip":"1.2.3.4","port":70000})"); }, "out of range: port"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"ip":"1.2.3.4","port":-1})"); }, "non-negative"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"transport":"rfcomm"})"); }, "bt (or name)"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"transport":"rfcomm","bt":"zz"})"); }, "Invalid Bluetooth"));
    CHECK(throwsWith([] { parseConnectionInfo("not json"); }, "not valid JSON"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"transport":"carrier-pigeon"})"); }, "Unsupported transport"));

    ConnectionInfo round;
    round.transport = "tcp";
    round.ip = "10.0.0.2";
    round.port = 9000;
    round.deviceName = "laptop";
    round.sessionToken = "0011";
    auto back = parseConnectionInfo(connectionInfoToJson(round));
    CHECK_EQ(back.ip, round.ip);
    CHECK_EQ(back.port, round.port);
    CHECK_EQ(back.sessionToken, round.sessionToken);
    CHECK(connectionInfoToJson(round).size() < 240);   // fits one NFC APDU
    CHECK(connectionInfoToJson(round).find("role") == std::string::npos);   // default stays implicit

    // A phone that has files publishes role "send" (the reader fetches them).
    auto sender = parseConnectionInfo(
        R"({"v":1,"role":"send","transport":"rfcomm","name":"Lakshya's M34","ip":"10.0.0.7","port":40123,"token":"ab12"})");
    CHECK_EQ(sender.role, std::string("send"));
    CHECK_EQ(sender.port, 40123);
    CHECK_EQ(parseConnectionInfo(connectionInfoToJson(sender)).role, std::string("send"));
    CHECK_EQ(bt.role, std::string("receive"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"role":"steal","ip":"1.2.3.4","port":1})"); }, "Unsupported role"));
    CHECK(throwsWith([] { parseConnectionInfo(R"({"role":"send","ip":"1.2.3.4","port":1})"); }, "must publish a token"));
}

static void testOfferLimits() {
    Limits limits;
    limits.maxFiles = 2;
    limits.maxTotalBytes = 100;
    auto status = [&](const std::string& json) {
        try {
            decodeOfferJson(json, limits);
            return OfferStatus::Accepted;
        } catch (const OfferError& e) {
            return e.status;
        }
    };
    CHECK(status(R"({"files":[{"name":"a","size":1}]})") == OfferStatus::Accepted);
    CHECK(status(R"({"files":[{"name":"a","size":1},{"name":"b","size":1},{"name":"c","size":1}]})") == OfferStatus::TooLarge);
    CHECK(status(R"({"files":[{"name":"a","size":101}]})") == OfferStatus::TooLarge);
    CHECK(status(R"({"files":[{"name":"a","size":-5}]})") == OfferStatus::BadRequest);
    CHECK(status(R"({"files":[{"name":"","size":1}]})") == OfferStatus::BadRequest);
    CHECK(status(R"({"files":[]})") == OfferStatus::BadRequest);
    CHECK(status(R"({"protocol":2,"files":[{"name":"a","size":1}]})") == OfferStatus::UnsupportedVersion);
    limits.maxTotalBytes = UINT64_MAX;
    limits.maxFileBytes = UINT64_MAX;
    CHECK(status(R"({"files":[{"name":"a","size":18446744073709551615},{"name":"b","size":2}]})") == OfferStatus::TooLarge);
}

static void testNoOverwrite() {
    TempDir dir;
    { std::ofstream(dir.path / "a.txt") << "original"; }
    for (int i = 0; i < 2; ++i) {
        fs::path tmp = dir.path / ("t" + std::to_string(i));
        { std::ofstream(tmp) << "new" << i; }
        commitWithoutOverwrite(tmp, dir.path, "a.txt");
    }
    std::ifstream orig(dir.path / "a.txt");
    std::string content;
    orig >> content;
    CHECK_EQ(content, std::string("original"));
    CHECK(fs::exists(dir.path / "a (1).txt"));
    CHECK(fs::exists(dir.path / "a (2).txt"));
}

// --- integration tests (real TCP over loopback) --------------------------------

static void testSmallAndEmptyAndMulti() {
    TempDir src, dst;
    writeRandomFile(src.path / "one.bin", 100000, 1);
    writeRandomFile(src.path / "empty.txt", 0, 2);
    writeRandomFile(src.path / "three.dat", 65536 * 3 + 17, 3);
    { std::ofstream(dst.path / "one.bin") << "pre-existing user file"; }

    ReceiverHarness rx(dst.path);
    rx.start();
    std::vector<OutgoingFile> files = {outgoingFileFromPath((src.path / "one.bin").string()),
                                       outgoingFileFromPath((src.path / "empty.txt").string()),
                                       outgoingFileFromPath((src.path / "three.dat").string())};
    SendResult sr = sendOver(rx.listener.port(), files, {"tester", "", 64 * 1024});
    rx.join();
    CHECK(sr.status == TransferStatus::Success);
    CHECK_EQ(sr.filesCompleted, 3u);
    CHECK_EQ(rx.results.size(), 1u);
    if (rx.results.empty()) return;
    const auto& r = rx.results[0];
    CHECK(r.status == TransferStatus::Success);
    CHECK_EQ(r.senderName, std::string("tester"));
    CHECK_EQ(r.files.size(), 3u);
    // Existing file untouched; new one saved alongside.
    std::ifstream pre(dst.path / "one.bin");
    std::string word;
    pre >> word;
    CHECK_EQ(word, std::string("pre-existing"));
    CHECK(fs::exists(dst.path / "one (1).bin"));
    CHECK_EQ(fileCrc(dst.path / "one (1).bin"), fileCrc(src.path / "one.bin"));
    CHECK(fs::exists(dst.path / "empty.txt") && fs::file_size(dst.path / "empty.txt") == 0);
    CHECK_EQ(fileCrc(dst.path / "three.dat"), fileCrc(src.path / "three.dat"));
    CHECK_EQ(countEntries(dst.path), 4u);   // no leftover .part files
}

static void testLargeFile() {
    const char* env = std::getenv("ANYSHARE_LARGE_MB");
    uint64_t mb = env ? std::strtoull(env, nullptr, 10) : 256;
    TempDir src, dst;
    fs::path big = src.path / "large.bin";
    writeRandomFile(big, mb << 20, 42);
    ReceiverHarness rx(dst.path);
    rx.start();
    auto t0 = std::chrono::steady_clock::now();
    SendResult sr = sendOver(rx.listener.port(), {outgoingFileFromPath(big.string())});
    rx.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(sr.status == TransferStatus::Success);
    CHECK(!rx.results.empty() && rx.results[0].status == TransferStatus::Success);
    CHECK_EQ(fileCrc(dst.path / "large.bin"), fileCrc(big));
    std::cout << "    " << mb << " MiB over loopback in " << secs << " s (" << (mb / secs)
              << " MiB/s, CRC verified on both ends)\n";
}

static void testPathTraversal() {
    TempDir src, dst;
    writeRandomFile(src.path / "payload", 10, 5);
    OutgoingFile f = outgoingFileFromPath((src.path / "payload").string());
    f.name = "../../escaped.txt";       // hostile name on the wire
    ReceiverHarness rx(dst.path / "inbox");
    rx.start();
    SendResult sr = sendOver(rx.listener.port(), {f});
    rx.join();
    CHECK(sr.status == TransferStatus::Success);
    CHECK(fs::exists(dst.path / "inbox" / "escaped.txt"));
    CHECK(!fs::exists(dst.path / "escaped.txt"));
    CHECK(!fs::exists(dst.path.parent_path() / "escaped.txt"));
}

static void testRejectAndTokens() {
    TempDir src, dst;
    writeRandomFile(src.path / "f", 10, 6);
    auto files = std::vector<OutgoingFile>{outgoingFileFromPath((src.path / "f").string())};
    {
        ReceiverHarness rx(dst.path);
        rx.options.acceptOffer = [](const Offer&) { return false; };
        rx.start();
        SendResult sr = sendOver(rx.listener.port(), files);
        rx.join();
        CHECK(sr.status == TransferStatus::Rejected);
        CHECK(sr.message.find("declined") != std::string::npos);
        CHECK_EQ(countEntries(dst.path), 0u);
    }
    {
        ReceiverHarness rx(dst.path);
        rx.options.acceptOffer = nullptr;        // only tap-authorised senders
        rx.options.expectedToken = "c0ffee00c0ffee00";
        rx.start(2);
        SendResult wrong = sendOver(rx.listener.port(), files, {"x", "deadbeef", 65536});
        SendResult right = sendOver(rx.listener.port(), files, {"x", "c0ffee00c0ffee00", 65536});
        rx.join();
        CHECK(wrong.status == TransferStatus::Rejected);
        CHECK(right.status == TransferStatus::Success);
        CHECK(rx.results.size() == 2 && rx.results[1].tokenMatched);
    }
    {
        ReceiverHarness rx(dst.path);
        rx.options.limits.maxTotalBytes = 5;
        rx.start();
        SendResult sr = sendOver(rx.listener.port(), files);
        rx.join();
        CHECK(sr.status == TransferStatus::Rejected);
        CHECK(sr.message.find("limit") != std::string::npos);
    }
}

static void testConnectionFailures() {
    uint16_t closedPort;
    {
        TcpListener probe("127.0.0.1", 0);
        closedPort = probe.port();
    }
    CHECK(throwsWith([&] { connectTcp("127.0.0.1", closedPort, 2000); }, "refused"));

    // Engine gives up after its connect window and reports it.
    struct Capture : IProgressObserver {
        std::string error;
        bool finished = false, success = true;
        void onProgress(int) override {}
        void onError(const std::string& m) override { error = m; }
        void onSessionFinished(bool ok, const std::string&) override { finished = true; success = ok; }
    } capture;
    TempDir src;
    writeRandomFile(src.path / "f", 10, 7);
    NetworkEngine engine;
    engine.addObserver(&capture);
    engine.setConnectTimeoutSeconds(3);
    engine.setRetryDelaySeconds(1);
    ConnectionInfo info;
    info.ip = "127.0.0.1";
    info.port = closedPort;
    info.files.push_back({(src.path / "f").string(), "", 0, ""});
    auto t0 = std::chrono::steady_clock::now();
    engine.connectToHost(info);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(capture.finished && !capture.success);
    CHECK(capture.error.find("Connection failed permanently") != std::string::npos);
    CHECK(secs < 6);

    // Missing source file: reported before any network activity.
    Capture missing;
    NetworkEngine engine2;
    engine2.addObserver(&missing);
    info.files = {{"/definitely/not/here.bin", "", 0, ""}};
    engine2.connectToHost(info);
    CHECK(!missing.success && missing.error.find("file not found") != std::string::npos);
    CHECK(throwsWith([] { outgoingFileFromPath("/"); }, "not a regular file"));
}

static void testInterruptedTransfer() {
    TempDir src, dst;
    writeRandomFile(src.path / "big.bin", 4 << 20, 8);
    ReceiverHarness rx(dst.path);
    rx.start();
    FaultyStream link(connectTcp("127.0.0.1", rx.listener.port(), 5000), 1 << 20);
    SendResult sr = sendFiles(link, {outgoingFileFromPath((src.path / "big.bin").string())}, {});
    rx.join();
    CHECK(sr.status == TransferStatus::TransportFailed);
    CHECK(sr.retryable());
    CHECK(!rx.results.empty() && rx.results[0].status == TransferStatus::TransportFailed);
    CHECK_EQ(countEntries(dst.path), 0u);   // partial data removed
}

static void testCorruptionDetected() {
    TempDir src, dst;
    writeRandomFile(src.path / "c.bin", 300000, 9);
    ReceiverHarness rx(dst.path);
    rx.start();
    // Corrupt one payload byte well past the OFFER/header.
    FaultyStream link(connectTcp("127.0.0.1", rx.listener.port(), 5000), UINT64_MAX, 200000);
    SendResult sr = sendFiles(link, {outgoingFileFromPath((src.path / "c.bin").string())}, {});
    rx.join();
    CHECK(sr.status == TransferStatus::IntegrityFailed);
    CHECK(!rx.results.empty() && rx.results[0].status == TransferStatus::IntegrityFailed);
    CHECK_EQ(countEntries(dst.path), 0u);
}

static void testEngineResumesAfterDrop() {
    TempDir src, dst;
    writeRandomFile(src.path / "a.bin", 200000, 10);
    writeRandomFile(src.path / "b.bin", 2 << 20, 11);
    writeRandomFile(src.path / "c.bin", 300000, 12);
    ReceiverHarness rx(dst.path);
    rx.start(2);

    int connections = 0;
    NetworkEngine engine;
    engine.setRetryDelaySeconds(1);
    engine.setStreamFactory([&](const ConnectionInfo& info, int timeoutMs) -> std::unique_ptr<IByteStream> {
        auto s = connectTcp(info.ip, info.port, timeoutMs);
        // First link dies in the middle of b.bin (after a.bin was acknowledged).
        if (++connections == 1) return std::make_unique<FaultyStream>(std::move(s), 1 << 20);
        return s;
    });
    // Unacknowledged bytes are resent, so progress may fall back -- but only to
    // the confirmed boundary (end of a.bin), never below it.
    struct Capture : IProgressObserver {
        int last = -1, lowestAfterDrop = 101, peak = -1;
        bool success = false;
        void onProgress(int p) override {
            if (p < last) lowestAfterDrop = std::min(lowestAfterDrop, p);
            peak = std::max(peak, p);
            last = p;
        }
        void onSessionFinished(bool ok, const std::string&) override { success = ok; }
    } capture;
    engine.addObserver(&capture);
    ConnectionInfo info;
    info.ip = "127.0.0.1";
    info.port = rx.listener.port();
    for (auto n : {"a.bin", "b.bin", "c.bin"}) info.files.push_back({(src.path / n).string(), "", 0, ""});
    engine.connectToHost(info);
    rx.join();
    CHECK(capture.success && engine.lastTransferSucceeded());
    CHECK_EQ(connections, 2);
    CHECK_EQ(capture.last, 100);
    uint64_t total = 200000 + (2 << 20) + 300000;
    CHECK(capture.lowestAfterDrop >= static_cast<int>(200000 * 100 / total) && capture.lowestAfterDrop < 100);
    // Every file exactly once, byte-identical.
    CHECK_EQ(countEntries(dst.path), 3u);
    for (auto n : {"a.bin", "b.bin", "c.bin"}) CHECK_EQ(fileCrc(dst.path / n), fileCrc(src.path / n));
}

static void testCancellation() {
    TempDir src, dst;
    writeRandomFile(src.path / "x.bin", 16 << 20, 13);
    ReceiverHarness rx(dst.path);
    rx.start();
    CancelToken cancel;
    struct CancelAt : IProgressObserver {
        CancelToken* token;
        void onProgress(int p) override {
            if (p >= 25) token->cancel();
        }
    } obs;
    obs.token = &cancel;
    SendResult sr = sendOver(rx.listener.port(), {outgoingFileFromPath((src.path / "x.bin").string())},
                             {"", "", 64 * 1024}, &obs, &cancel);
    rx.join();
    CHECK(sr.status == TransferStatus::Cancelled);
    CHECK(!rx.results.empty() && rx.results[0].status != TransferStatus::Success);
    CHECK_EQ(countEntries(dst.path), 0u);
}

static void testGarbagePeer() {
    TempDir dst;
    ReceiverHarness rx(dst.path);
    rx.start();
    auto s = connectTcp("127.0.0.1", rx.listener.port(), 5000);
    const char* http = "GET / HTTP/1.1\r\n\r\n";
    s->writeAll(http, std::strlen(http));
    std::string reason;
    OfferStatus st = readResponse(*s, &reason);
    rx.join();
    CHECK(st == OfferStatus::BadRequest);
    CHECK(!rx.results.empty() && rx.results[0].status == TransferStatus::ProtocolFailed);
}

// The phone with the files listens; the laptop (after reading its tap) connects and pulls.
static void testPull() {
    TempDir src, dst;
    writeRandomFile(src.path / "photo.jpg", 700000, 14);
    writeRandomFile(src.path / "notes.txt", 1234, 15);
    std::vector<OutgoingFile> files{outgoingFileFromPath((src.path / "photo.jpg").string()),
                                    outgoingFileFromPath((src.path / "notes.txt").string())};
    const std::string token = "0123456789abcdef";

    TcpListener phone("127.0.0.1", 0);
    std::vector<PullSendResult> served;
    std::thread phoneThread([&] {
        for (int i = 0; i < 3; ++i) {   // a stranger, a garbage peer, then the laptop that tapped
            auto s = phone.accept(15000);
            if (!s) break;
            SendOptions opts;
            opts.senderName = "Lakshya's M34";
            opts.token = token;
            served.push_back(sendFilesOnPull(*s, files, opts));
            if (served.back().status == TransferStatus::Success) break;
        }
    });

    ReceiveOptions options;
    options.destinationDir = dst.path;
    options.reserveBytes = 0;
    options.acceptOffer = [](const Offer&) { return false; };   // only the tap token may authorise
    {   // someone on the same Wi-Fi who did not read the tap: gets nothing, not even file names
        auto s = connectTcp("127.0.0.1", phone.port(), 5000);
        ReceiveResult stranger = receiveFilesByPull(*s, "ffffffffffffffff", "intruder", options);
        CHECK(stranger.status != TransferStatus::Success);
        CHECK(stranger.files.empty());
    }
    {   // not speaking ASP at all
        auto s = connectTcp("127.0.0.1", phone.port(), 5000);
        const char* http = "GET / HTTP/1.1\r\n\r\n";
        s->writeAll(http, std::strlen(http));
        char c;
        try { s->readSome(&c, 1); } catch (const std::exception&) {}
    }
    auto s = connectTcp("127.0.0.1", phone.port(), 5000);
    ReceiveResult got = receiveFilesByPull(*s, token, "NILAY", options);
    phoneThread.join();

    CHECK(got.status == TransferStatus::Success);
    CHECK(got.tokenMatched);
    CHECK_EQ(got.senderName, std::string("Lakshya's M34"));
    CHECK_EQ(got.files.size(), 2u);
    CHECK_EQ(fileCrc(dst.path / "photo.jpg"), fileCrc(src.path / "photo.jpg"));
    CHECK_EQ(fileCrc(dst.path / "notes.txt"), fileCrc(src.path / "notes.txt"));
    CHECK_EQ(served.size(), 3u);
    if (served.size() == 3) {
        CHECK(served[0].pullRefused && served[0].filesCompleted == 0);
        CHECK(served[1].pullRefused);
        CHECK(!served[2].pullRefused && served[2].status == TransferStatus::Success);
        CHECK_EQ(served[2].receiverName, std::string("NILAY"));
        CHECK_EQ(served[2].filesCompleted, 2u);
    }
}

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"crc32", testCrc32},
        {"sanitize filenames", testSanitize},
        {"handover parser (all schemas)", testHandoverParser},
        {"offer limits", testOfferLimits},
        {"no-overwrite commit", testNoOverwrite},
        {"tcp: small + empty + multi + collision", testSmallAndEmptyAndMulti},
        {"tcp: large file", testLargeFile},
        {"tcp: path traversal", testPathTraversal},
        {"tcp: reject / token / size limit", testRejectAndTokens},
        {"tcp: connection failures", testConnectionFailures},
        {"tcp: interrupted transfer", testInterruptedTransfer},
        {"tcp: corruption detected", testCorruptionDetected},
        {"engine: resume after drop", testEngineResumesAfterDrop},
        {"tcp: cancellation", testCancellation},
        {"tcp: garbage peer", testGarbagePeer},
        {"tcp: pull (receiver connects to the sender)", testPull},
    };
    for (const auto& t : tests) {
        int before = g_failures;
        std::cout << "[ RUN  ] " << t.name << std::endl;
        t.fn();
        std::cout << (g_failures == before ? "[  OK  ] " : "[ FAIL ] ") << t.name << std::endl;
    }
    std::cout << "\n" << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
