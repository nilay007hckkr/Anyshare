#pragma once

// Finds AnyShare receivers on the local network by name, so nobody has to
// type an IP address. Plain UDP broadcast: no Avahi/mDNS daemon needed.
//
//   sender   -> broadcast   "ANYSHARE?1"                         to UDP 48080
//   receiver -> unicast     "ANYSHARE!1|<tcp port>|<transport>|<os>|<name>"
//
// The receiver's IP is the reply's source address. Replies carry no session
// token, so a transfer found this way still needs the receiver's Accept.
// The same format is implemented in Java (android .../transfer/LanDiscovery.java).
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace anyshare::discovery {

constexpr uint16_t kPort = 48080;
constexpr const char* kRequest = "ANYSHARE?1";
constexpr const char* kReplyPrefix = "ANYSHARE!1|";

struct Peer {
    std::string name;
    std::string host;          // source address of the reply
    uint16_t port = 0;         // the receiver's TCP port
    std::string transport = "tcp";
    std::string os;            // "linux", "android", ...
};

// Control characters removed, at most 64 bytes, never cut inside a UTF-8 character.
std::string cleanName(const std::string& name);
std::string encodeReply(const std::string& name, uint16_t tcpPort, const std::string& os);
// Fills everything except `host`. Returns false for anything that is not a valid reply.
bool parseReply(const std::string& datagram, Peer& peer);

// Answers discovery requests while it lives. Never throws: if the UDP port is
// taken (e.g. another receiver on this computer answers already), error() says so.
class Responder {
public:
    Responder(std::string name, uint16_t tcpPort, std::string os = "linux", uint16_t udpPort = kPort);
    ~Responder();
    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;

    bool running() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    uint16_t udpPort() const { return udpPort_; }   // the bound port (useful when 0 was requested)

private:
    std::string reply_;
    int fd_ = -1;
    uint16_t udpPort_ = 0;
    std::string error_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Broadcasts a request on every IPv4 interface (and to `extraTargets`, e.g.
// "127.0.0.1" in tests), repeats it twice for lossy Wi-Fi, and returns the
// receivers that answered within timeoutMs, one entry per name and port.
std::vector<Peer> discover(int timeoutMs = 1500, uint16_t udpPort = kPort,
                           const std::vector<std::string>& extraTargets = {});

}  // namespace anyshare::discovery
