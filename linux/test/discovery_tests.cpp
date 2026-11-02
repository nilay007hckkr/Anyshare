// Tests for finding receivers by name (LanDiscovery). Loopback UDP only:
//   ./build-linux/discovery_tests
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "LanDiscovery.hpp"

using namespace anyshare::discovery;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

void testFormat() {
    CHECK(encodeReply("nilay-laptop", 8080, "linux") == "ANYSHARE!1|8080|tcp|linux|nilay-laptop");
    Peer p;
    CHECK(parseReply("ANYSHARE!1|8080|tcp|android|Almas's Galaxy | A52", p));
    CHECK(p.port == 8080 && p.os == "android" && p.name == "Almas's Galaxy | A52");   // '|' allowed in the name
    CHECK(!parseReply("ANYSHARE!1|99999|tcp|linux|x", p));    // bad port
    CHECK(!parseReply("ANYSHARE!1|8080|udp|linux|x", p));     // unknown transport
    CHECK(!parseReply("ANYSHARE!1|8080|tcp", p));             // truncated
    CHECK(!parseReply("HELLO", p));
    CHECK(cleanName("bad\nname\t!") == "badname!");
    CHECK(cleanName("") == "AnyShare device");
    std::string longName;
    for (int i = 0; i < 40; ++i) longName += "\xC3\xA9";   // 80 bytes of 'é'
    std::string cut = cleanName(longName);
    CHECK(cut.size() == 64 && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);   // ends on a whole character
}

void testFindsResponder() {
    Responder r("test-laptop", 8123, "linux", 0);
    CHECK(r.running());
    auto start = std::chrono::steady_clock::now();
    auto peers = discover(600, r.udpPort(), {"127.0.0.1"});
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    CHECK(peers.size() == 1);   // three requests, still one entry
    if (!peers.empty()) {
        CHECK(peers[0].name == "test-laptop");
        CHECK(peers[0].port == 8123);
        CHECK(!peers[0].host.empty());   // 127.0.0.1, or our LAN IP when the broadcast answer wins
        CHECK(peers[0].os == "linux");
    }
}

void testIgnoresGarbage() {
    Responder r("sturdy", 9000, "linux", 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(r.udpPort());
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    const char* junk[] = {"", "ANYSHARE?", "ANYSHARE?2", "GET / HTTP/1.1\r\n\r\n"};
    for (const char* j : junk) sendto(fd, j, std::string(j).size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    close(fd);
    auto peers = discover(400, r.udpPort(), {"127.0.0.1"});
    CHECK(peers.size() == 1 && peers[0].name == "sturdy");   // still answers real requests
}

void testNothingThere() {
    uint16_t freePort;
    {
        Responder probe("x", 1, "linux", 0);   // find a free UDP port, then release it
        freePort = probe.udpPort();
    }
    auto start = std::chrono::steady_clock::now();
    CHECK(discover(300, freePort, {"127.0.0.1"}).empty());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
}

void testPortInUseIsReported() {
    Responder first("a", 1, "linux", 0);
    // SO_REUSEADDR lets a second socket share the port on Linux, so take it exclusively instead.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    Responder second("b", 1, "linux", ntohs(a.sin_port));
    CHECK(!second.running());
    CHECK(second.error().find("UDP port") != std::string::npos);
    close(fd);
}

int main() {
    testFormat();
    testFindsResponder();
    testIgnoresGarbage();
    testNothingThere();
    testPortInUseIsReported();
    std::printf("discovery_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
