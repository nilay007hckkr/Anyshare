#pragma once

#include "INfcHandover.hpp"
#include <iostream>

class LargeFileTestHandover : public INfcHandover {
public:
    std::string describe() const override { return "simulated handover (hardcoded 2 GB stress-test values)"; }

    ConnectionInfo performHandover() override {
        std::cout << "[LargeFileTestHandover] Preparing 2GB stress test..." << std::endl;

        ConnectionInfo info;
        info.ssid = "DIRECT-xy-AndroidHost";
        info.psk = "GeneratedPassword123";
        info.ip = "127.0.0.1";
        info.port = 8080;

        FileMetadata file;
        file.filename = "test_large.bin";
        file.mimeType = "application/octet-stream";
        file.filesize = 2147483648;
        file.checksum = "4dbdf21c";
        info.files.push_back(file);

        return info;
    }
};
