#include "LanDiscovery.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <set>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace anyshare::discovery {

namespace {

bool sendTo(int fd, const std::string& data, const sockaddr_in& to) {
    return sendto(fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) >= 0;
}

sockaddr_in ipv4(in_addr_t addr, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = htons(port);
    return a;
}

std::vector<in_addr_t> broadcastAddresses() {
    std::vector<in_addr_t> out = {htonl(INADDR_BROADCAST)};
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
        if (!i->ifa_broadaddr) continue;
        in_addr_t b = reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr;
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    freeifaddrs(list);
    return out;
}

}  // namespace

std::string cleanName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) continue;
        out += static_cast<char>(c);
    }
    if (out.size() > 64) {
        std::size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // stay on a character boundary
        out.resize(cut);
    }
    return out.empty() ? "AnyShare device" : out;
}

std::string encodeReply(const std::string& name, uint16_t tcpPort, const std::string& os) {
    return std::string(kReplyPrefix) + std::to_string(tcpPort) + "|tcp|" + os + "|" + cleanName(name);
}

bool parseReply(const std::string& datagram, Peer& peer) {
    const std::string prefix = kReplyPrefix;
    if (datagram.compare(0, prefix.size(), prefix) != 0) return false;
    std::vector<std::string> fields;
    std::size_t start = prefix.size();
    for (int i = 0; i < 3; ++i) {   // port, transport, os; the name is the rest and may contain '|'
        std::size_t bar = datagram.find('|', start);
        if (bar == std::string::npos) return false;
        fields.push_back(datagram.substr(start, bar - start));
        start = bar + 1;
    }
    const std::string& portText = fields[0];
    if (portText.empty() || portText.size() > 5 ||
        !std::all_of(portText.begin(), portText.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }
    int port = std::stoi(portText);
    if (port < 1 || port > 65535 || fields[1] != "tcp") return false;
    peer.port = static_cast<uint16_t>(port);
    peer.transport = fields[1];
    peer.os = fields[2];
    peer.name = cleanName(datagram.substr(start));
    return true;
}

// ---------------------------------------------------------------------------
Responder::Responder(std::string name, uint16_t tcpPort, std::string os, uint16_t udpPort)
    : reply_(encodeReply(name, tcpPort, os)) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error_ = std::string("UDP socket: ") + std::strerror(errno);
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = ipv4(htonl(INADDR_ANY), udpPort);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "UDP port " + std::to_string(udpPort) + ": " + std::strerror(errno);
        close(fd);
        return;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    udpPort_ = ntohs(addr.sin_port);
    fd_ = fd;
    thread_ = std::thread([this] {
        char buf[512];
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 200) <= 0) continue;
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) continue;
            std::string request(buf, static_cast<std::size_t>(n));
            while (!request.empty() && std::isspace(static_cast<unsigned char>(request.back()))) request.pop_back();
            if (request == kRequest) sendTo(fd_, reply_, from);
        }
    });
}

Responder::~Responder() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) close(fd_);
}

// ---------------------------------------------------------------------------
std::vector<Peer> discover(int timeoutMs, uint16_t udpPort, const std::vector<std::string>& extraTargets) {
    std::vector<Peer> peers;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return peers;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    std::vector<sockaddr_in> targets;
    for (in_addr_t b : broadcastAddresses()) targets.push_back(ipv4(b, udpPort));
    for (const auto& host : extraTargets) {
        in_addr a{};
        if (inet_pton(AF_INET, host.c_str(), &a) == 1) targets.push_back(ipv4(a.s_addr, udpPort));
    }

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto deadline = start + std::chrono::milliseconds(timeoutMs);
    int sent = 0;
    std::set<std::string> seen;
    for (;;) {
        auto now = clock::now();
        if (now >= deadline) break;
        // Three requests spread over the window: Wi-Fi drops broadcast packets.
        if (sent < 3 && now >= start + std::chrono::milliseconds(timeoutMs * sent / 3)) {
            for (const auto& t : targets) sendTo(fd, kRequest, t);   // errors ignored: some interfaces refuse broadcast
            ++sent;
        }
        int waitMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, std::min(waitMs, 100)) <= 0) continue;
        char buf[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;
        Peer peer;
        if (!parseReply(std::string(buf, static_cast<std::size_t>(n)), peer)) continue;
        char host[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
        peer.host = host;
        if (seen.insert(peer.name + "|" + std::to_string(peer.port)).second) peers.push_back(peer);
    }
    close(fd);
    std::sort(peers.begin(), peers.end(), [](const Peer& a, const Peer& b) { return a.name < b.name; });
    return peers;
}

}  // namespace anyshare::discovery
