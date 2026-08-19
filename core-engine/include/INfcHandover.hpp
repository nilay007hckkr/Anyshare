#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct FileMetadata {
    std::string filename;
    std::string mimeType;
    uint64_t filesize;
    std::string checksum;
};

struct ConnectionInfo {
    std::string ssid;
    std::string psk;
    std::string ip;
    uint16_t port;
    std::vector<FileMetadata> files;
};

class INfcHandover {
public:
    virtual ConnectionInfo performHandover() = 0;
    virtual ~INfcHandover() = default;
};
