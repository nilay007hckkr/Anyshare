#pragma once

// Small helpers shared by the command-line tool and the Qt desktop app.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#include "ConnectionInfoParser.hpp"
#include "INfcHandover.hpp"

namespace anyshare::host {

inline std::string hostName() {
    char name[256] = "linux";
    gethostname(name, sizeof(name) - 1);
    return name;
}

inline std::string firstLanIpv4() {
    ifaddrs* list = nullptr;
    std::string result;
    if (getifaddrs(&list) != 0) return result;
    for (ifaddrs* i = list; i && result.empty(); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, text, sizeof(text));
        result = text;
    }
    freeifaddrs(list);
    return result;
}

// True inside WSL, where phones must use the Windows host's Wi-Fi address.
inline bool runningUnderWsl() {
    std::ifstream in("/proc/sys/kernel/osrelease");
    std::string release;
    std::getline(in, release);
    std::transform(release.begin(), release.end(), release.begin(), [](unsigned char c) { return std::tolower(c); });
    return release.find("microsoft") != std::string::npos;
}

inline std::string humanBytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

inline void splitHostPort(const std::string& text, std::string& host, uint16_t& port) {
    auto colon = text.rfind(':');
    std::string portText = colon == std::string::npos ? "" : text.substr(colon + 1);
    bool digits = !portText.empty() && portText.size() <= 5 &&
                  std::all_of(portText.begin(), portText.end(), [](unsigned char c) { return std::isdigit(c); });
    if (colon == 0 || !digits || std::stoi(portText) < 1 || std::stoi(portText) > 65535) {
        throw std::invalid_argument("expected HOST:PORT (e.g. 192.168.1.5:8080), got '" + text + "'");
    }
    host = text.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(portText));
}

inline ConnectionInfo tcpTarget(const std::string& hostPort) {
    ConnectionInfo info;
    info.transport = "tcp";
    splitHostPort(hostPort, info.ip, info.port);
    return info;
}

// "AA:BB:CC:DD:EE:FF" or "AA:BB:CC:DD:EE:FF@CHANNEL" (channel 0 / absent = SDP lookup).
inline ConnectionInfo bluetoothTarget(const std::string& spec) {
    ConnectionInfo info;
    info.transport = "rfcomm";
    auto at = spec.find('@');
    info.btAddress = spec.substr(0, at);
    if (!isValidBluetoothAddress(info.btAddress)) throw std::invalid_argument("invalid Bluetooth address " + info.btAddress);
    if (at != std::string::npos) {
        int channel = std::stoi(spec.substr(at + 1));
        if (channel < 0 || channel > 30) throw std::invalid_argument("RFCOMM channel must be 1-30");
        info.rfcommChannel = static_cast<uint8_t>(channel);
    }
    return info;
}

// A Bluetooth address (optionally @channel) selects RFCOMM; anything else must be HOST:PORT.
inline ConnectionInfo parseTarget(const std::string& text) {
    std::string t = text;
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    if (isValidBluetoothAddress(t.substr(0, t.find('@')))) return bluetoothTarget(t);
    return tcpTarget(t);
}

// A tap payload can name a Bluetooth receiver *and* carry its Wi-Fi address.
inline bool hasWifiAlternative(const ConnectionInfo& info) {
    return info.transport == "rfcomm" && !info.ip.empty() && info.port != 0;
}

}  // namespace anyshare::host
