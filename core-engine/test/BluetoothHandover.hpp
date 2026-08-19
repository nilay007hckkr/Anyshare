#pragma once

#include "INfcHandover.hpp"
#include <iostream>

class BluetoothHandover : public INfcHandover {
public:
    ConnectionInfo performHandover() override {
        std::cout << "[BluetoothHandover] Simulating Bluetooth RFCOMM pairing..." << std::endl;

        ConnectionInfo info;
        info.ssid = "DIRECT-xy-AndroidHost";
        info.psk = "GeneratedPassword123";
        info.ip = "127.0.0.1";
        info.port = 8080;

        FileMetadata file1;
        file1.filename = "test1.txt";
        file1.mimeType = "text/plain";
        file1.filesize = 62;
        file1.checksum = "42431e77";
        info.files.push_back(file1);

        std::cout << "[BluetoothHandover] Connection info received." << std::endl;
        return info;
    }
};
