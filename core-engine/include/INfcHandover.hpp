#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FileMetadata {
    // Sender side: local path of the file to read. Only the leaf name is sent
    // on the wire; receivers never see (or trust) directory components.
    std::string filename;
    std::string mimeType = "application/octet-stream";
    uint64_t filesize = 0;      // informational; the real size is taken from the file
    std::string checksum;       // optional expected CRC32 (hex). If set, the sender
                                // verifies the bytes it read match it.
};

// Everything a sender needs to reach a receiver. Produced by a handover
// strategy (physical NFC tap, simulated tap, manual entry, Bluetooth picker).
struct ConnectionInfo {
    // Optional Wi-Fi Direct credentials (only meaningful when joining a P2P group).
    std::string ssid;
    std::string psk;

    // Payload transport: "tcp" (Wi-Fi / LAN / Wi-Fi Direct) or "rfcomm" (Bluetooth).
    std::string transport = "tcp";
    std::string ip;
    uint16_t port = 0;
    std::string btAddress;       // "AA:BB:CC:DD:EE:FF"; Android cannot report its own MAC,
    uint8_t rfcommChannel = 0;   // so it advertises deviceName instead. 0 = resolve via SDP.

    std::string deviceName;      // receiver's display / Bluetooth name
    std::string sessionToken;    // one-time token: proves the sender read the receiver's tap payload

    // What the device that published this payload wants:
    //   "receive" -- it waits for files: connect to it and send (the usual case).
    //   "send"    -- it has files and waits for the reader to connect and fetch them ("pull").
    //                Used when a phone is tapped on a reader that cannot act as a card (ESP32 + Si512).
    std::string role = "receive";

    std::vector<FileMetadata> files;   // files to send (sender side)
};

// Strategy interface: how the sender learns where the receiver is.
class INfcHandover {
public:
    virtual ConnectionInfo performHandover() = 0;

    // How the handover was obtained, for diagnostics. Implementations must be
    // truthful, e.g. "physical NFC (PN532 via libnfc)" vs "simulated tap (JSON file)".
    virtual std::string describe() const { return "unspecified handover"; }

    virtual ~INfcHandover() = default;
};
