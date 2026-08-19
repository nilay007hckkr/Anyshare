#pragma once

#include "INfcHandover.hpp"
#include "ConnectionInfoParser.hpp"
#include <iostream>

class JsonTestHandover : public INfcHandover {
public:
    ConnectionInfo performHandover() override {
        std::cout << "[JsonTestHandover] Simulating raw JSON payload received via tap..." << std::endl;

        std::string rawJson = R"({
            "ssid": "DIRECT-xy-AndroidHost",
            "psk": "GeneratedPassword123",
            "ip": "127.0.0.1",
            "port": 8080,
            "files": [
                {
                    "filename": "test1.txt",
                    "mimeType": "text/plain",
                    "filesize": 62,
                    "checksum": "42431e77"
                }
            ]
        })";

        ConnectionInfo info = parseConnectionInfo(rawJson);
        std::cout << "[JsonTestHandover] Parsed " << info.files.size() << " file(s) from JSON." << std::endl;
        return info;
    }
};
