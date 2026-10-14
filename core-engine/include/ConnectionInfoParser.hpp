#pragma once

#include <cctype>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include "INfcHandover.hpp"

// Handover payload (NFC HCE response, PN532 read, simulated-tap file, QR, ...).
//
// Accepted schemas, so payloads from every existing branch keep working:
//   v1 (current) : {"v":1,"transport":"rfcomm","bt":"AA:..","ch":0,"name":"..","token":".."}
//                  {"v":1,"transport":"tcp","ip":"192.168.1.5","port":8080,"name":"..","token":".."}
//                  optional "role":"send" = the publisher has files and waits to be fetched (default "receive")
//   main (multi) : {"ssid","psk","ip","port","files":[{"filename","mimeType","filesize","checksum"}]}
//   Android-Almas: {"ssid","psk","ip","port","filename","filesize"}
namespace anyshare {

constexpr std::size_t kMaxHandoverJsonBytes = 64 * 1024;

inline bool isValidBluetoothAddress(const std::string& a) {
    if (a.size() != 17) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 3 == 2) {
            if (a[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(a[i]))) {
            return false;
        }
    }
    return true;
}

namespace detail {

inline std::string requireString(const nlohmann::json& j, const char* field) {
    if (!j.contains(field)) throw std::invalid_argument(std::string("Missing required field: ") + field);
    if (!j.at(field).is_string()) throw std::invalid_argument(std::string("Field must be a string: ") + field);
    return j.at(field).get<std::string>();
}

inline std::string optionalString(const nlohmann::json& j, const char* field, std::string fallback = "") {
    if (!j.contains(field) || j.at(field).is_null()) return fallback;
    if (!j.at(field).is_string()) throw std::invalid_argument(std::string("Field must be a string: ") + field);
    return j.at(field).get<std::string>();
}

inline uint64_t optionalUnsigned(const nlohmann::json& j, const char* field, uint64_t max, uint64_t fallback = 0) {
    if (!j.contains(field) || j.at(field).is_null()) return fallback;
    const auto& v = j.at(field);
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<int64_t>() < 0)) {
        throw std::invalid_argument(std::string("Field must be a non-negative integer: ") + field);
    }
    uint64_t value = v.get<uint64_t>();
    if (value > max) throw std::invalid_argument(std::string("Field out of range: ") + field);
    return value;
}

inline FileMetadata parseFile(const nlohmann::json& f) {
    if (!f.is_object()) throw std::invalid_argument("Each entry of 'files' must be an object");
    FileMetadata file;
    file.filename = requireString(f, "filename");
    file.mimeType = optionalString(f, "mimeType", "application/octet-stream");
    file.filesize = optionalUnsigned(f, "filesize", UINT64_MAX);
    file.checksum = optionalString(f, "checksum");
    if (!file.checksum.empty()) {
        bool hex = file.checksum.size() == 8;
        for (char c : file.checksum) hex = hex && std::isxdigit(static_cast<unsigned char>(c));
        if (!hex) throw std::invalid_argument("checksum must be 8 hex digits (CRC32)");
    }
    return file;
}

}  // namespace detail

inline ConnectionInfo parseConnectionInfo(const std::string& jsonText) {
    if (jsonText.size() > kMaxHandoverJsonBytes) {
        throw std::invalid_argument("Handover payload too large");
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::invalid_argument(std::string("Handover payload is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::invalid_argument("Handover payload must be a JSON object");

    using namespace detail;
    ConnectionInfo info;
    info.ssid = optionalString(j, "ssid");
    info.psk = optionalString(j, "psk");
    info.btAddress = optionalString(j, "bt");
    info.deviceName = optionalString(j, "name");
    info.sessionToken = optionalString(j, "token");
    info.role = optionalString(j, "role", "receive");
    if (info.role != "receive" && info.role != "send") throw std::invalid_argument("Unsupported role: " + info.role);
    if (info.role == "send" && info.sessionToken.empty()) {
        throw std::invalid_argument("A sending device must publish a token");   // it is what authorises the fetch
    }

    std::string transport = optionalString(j, "transport");
    if (transport.empty()) {
        transport = (!j.contains("ip") && (j.contains("bt") || j.contains("name"))) ? "rfcomm" : "tcp";
    }
    if (transport != "tcp" && transport != "rfcomm") {
        throw std::invalid_argument("Unsupported transport: " + transport);
    }
    info.transport = transport;

    if (transport == "tcp") {
        info.ip = requireString(j, "ip");
        if (!j.contains("port")) throw std::invalid_argument("Missing required field: port");
        info.port = static_cast<uint16_t>(optionalUnsigned(j, "port", 65535));
        if (info.port == 0) throw std::invalid_argument("Field out of range: port");
    } else {
        // Receivers may also advertise a Wi-Fi TCP endpoint as an alternative.
        info.ip = optionalString(j, "ip");
        info.port = static_cast<uint16_t>(optionalUnsigned(j, "port", 65535));
        info.rfcommChannel = static_cast<uint8_t>(optionalUnsigned(j, "ch", 30));
        if (info.btAddress.empty() && info.deviceName.empty()) {
            throw std::invalid_argument("Missing required field: bt (or name)");
        }
        if (!info.btAddress.empty() && !isValidBluetoothAddress(info.btAddress)) {
            throw std::invalid_argument("Invalid Bluetooth address: " + info.btAddress);
        }
    }

    if (info.deviceName.size() > 248) throw std::invalid_argument("Field too long: name");
    if (info.sessionToken.size() > 64) throw std::invalid_argument("Field too long: token");

    if (j.contains("files")) {
        if (!j.at("files").is_array()) throw std::invalid_argument("Field must be an array: files");
        for (const auto& f : j.at("files")) info.files.push_back(parseFile(f));
    } else if (j.contains("filename")) {
        // Legacy single-file payload from the original Android branch.
        FileMetadata file;
        file.filename = requireString(j, "filename");
        file.filesize = optionalUnsigned(j, "filesize", UINT64_MAX);
        file.mimeType = optionalString(j, "mimeType", "application/octet-stream");
        info.files.push_back(file);
    }
    return info;
}

// Compact serialisation used by receivers to advertise themselves (fits in a
// single NFC APDU response for typical names).
inline std::string connectionInfoToJson(const ConnectionInfo& info, bool includeFiles = false) {
    nlohmann::json j;
    j["v"] = 1;
    j["transport"] = info.transport;
    if (info.role != "receive") j["role"] = info.role;   // omitted by default: older readers stay compatible
    if (!info.ip.empty()) j["ip"] = info.ip;
    if (info.port != 0) j["port"] = info.port;
    if (!info.btAddress.empty()) j["bt"] = info.btAddress;
    if (info.rfcommChannel != 0) j["ch"] = info.rfcommChannel;
    if (!info.deviceName.empty()) j["name"] = info.deviceName;
    if (!info.sessionToken.empty()) j["token"] = info.sessionToken;
    if (!info.ssid.empty()) j["ssid"] = info.ssid;
    if (!info.psk.empty()) j["psk"] = info.psk;
    if (includeFiles) {
        j["files"] = nlohmann::json::array();
        for (const auto& f : info.files) {
            nlohmann::json fj{{"filename", f.filename}, {"mimeType", f.mimeType}, {"filesize", f.filesize}};
            if (!f.checksum.empty()) fj["checksum"] = f.checksum;
            j["files"].push_back(fj);
        }
    }
    return j.dump();
}

}  // namespace anyshare

// Backwards-compatible global name used by the original tests.
using anyshare::parseConnectionInfo;
