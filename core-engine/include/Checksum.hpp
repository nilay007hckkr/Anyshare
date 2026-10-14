#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) -- identical values to
// zlib's crc32() and java.util.zip.CRC32. Slicing-by-8 keeps checksumming far
// faster than any wireless link so it never becomes the transfer bottleneck.
//
// CRC-32 detects accidental corruption; it is not a defence against a
// deliberate attacker (that is the job of the transport's link encryption).
class Crc32Calculator {
private:
    using Table = std::array<std::array<uint32_t, 256>, 8>;

    // Function-local static: initialised exactly once, thread-safe (C++11).
    static const Table& tables() {
        static const Table table = [] {
            Table t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[0][i] = c;
            }
            for (uint32_t i = 0; i < 256; ++i) {
                for (int s = 1; s < 8; ++s) {
                    t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
                }
            }
            return t;
        }();
        return table;
    }

    uint32_t crc_ = 0xFFFFFFFFu;

public:
    void update(const char* data, size_t length) {
        update(reinterpret_cast<const uint8_t*>(data), length);
    }

    void update(const uint8_t* p, size_t length) {
        const Table& t = tables();
        uint32_t c = crc_;
        while (length >= 8) {
            uint32_t lo = c ^ (uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                               (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
            uint32_t hi = uint32_t(p[4]) | (uint32_t(p[5]) << 8) |
                          (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
            c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^
                t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
                t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^
                t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
            p += 8;
            length -= 8;
        }
        while (length--) {
            c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
        }
        crc_ = c;
    }

    uint32_t value() const { return crc_ ^ 0xFFFFFFFFu; }

    std::string getHexValue() const { return toHex(value()); }

    static std::string toHex(uint32_t crc) {
        char text[9];
        std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(crc));
        return text;
    }
};
