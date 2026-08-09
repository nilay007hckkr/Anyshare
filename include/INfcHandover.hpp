#pragma once
// ============================================================================
// INfcHandover.hpp — Strategy Pattern (GoF)
// ============================================================================
// OOP Principle: ABSTRACTION + POLYMORPHISM
// This pure virtual interface defines a contract for obtaining connection
// credentials. Different concrete strategies can implement this:
//   - JsonHandover:        parses a raw JSON string (for testing / demo)
//   - LinuxNfcHandover:    reads from PN532 hardware via libnfc (Member 2)
//   - AndroidNfcHandover:  reads from Android NFC APIs via JNI (Member 3)
// The NetworkEngine calls performHandover() without knowing which concrete
// strategy is plugged in — classic Strategy pattern.
// ============================================================================

#include <string>
#include <cstdint>

// OOP Principle: ENCAPSULATION
// Groups all connection-related data into a single cohesive struct.
// Fields are public (Plain Old Data) since this is a data-transfer object,
// not a behavior-bearing class.
struct ConnectionInfo {
    std::string ssid;       // Wi-Fi Direct network name
    std::string psk;        // Pre-shared key for the network
    std::string ip;         // Target IP (typically 192.168.49.1)
    uint16_t    port;       // Target TCP port (typically 8080)
    std::string filename;   // Name of the file to transfer
    uint64_t    filesize;   // Size of the file in bytes
};

class INfcHandover {
public:
    // Strategy method: concrete implementations obtain ConnectionInfo
    // from their respective NFC hardware/source.
    virtual ConnectionInfo performHandover() = 0;

    virtual ~INfcHandover() = default;
};
