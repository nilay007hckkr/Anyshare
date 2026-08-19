#pragma once

#include <cstdint>
#include <cstddef>
#include <sstream>
#include <iomanip>

class Crc32Calculator {
private:
    uint32_t crc_ = 0xFFFFFFFF;
    inline static uint32_t table_[256];
    inline static bool tableInitialized_ = false;

    static void initTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            table_[i] = c;
        }
        tableInitialized_ = true;
    }

public:
    Crc32Calculator() {
        if (!tableInitialized_) initTable();
    }

    void update(const char* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            uint8_t byte = static_cast<uint8_t>(data[i]);
            crc_ = table_[(crc_ ^ byte) & 0xFF] ^ (crc_ >> 8);
        }
    }

    std::string getHexValue() const {
        uint32_t finalCrc = crc_ ^ 0xFFFFFFFF;
        std::stringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(8) << finalCrc;
        return ss.str();
    }
};
