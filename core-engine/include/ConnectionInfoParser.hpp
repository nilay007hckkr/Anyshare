#pragma once

#include <nlohmann/json.hpp>
#include "INfcHandover.hpp"

inline ConnectionInfo parseConnectionInfo(const std::string& jsonText) {
    nlohmann::json parsed = nlohmann::json::parse(jsonText);

    ConnectionInfo info;
    info.ssid = parsed.at("ssid").get<std::string>();
    info.psk = parsed.at("psk").get<std::string>();
    info.ip = parsed.at("ip").get<std::string>();
    info.port = parsed.at("port").get<uint16_t>();

    for (const auto& fileJson : parsed.at("files")) {
        FileMetadata file;
        file.filename = fileJson.at("filename").get<std::string>();
        file.mimeType = fileJson.at("mimeType").get<std::string>();
        file.filesize = fileJson.at("filesize").get<uint64_t>();
        file.checksum = fileJson.at("checksum").get<std::string>();
        info.files.push_back(file);
    }

    return info;
}
