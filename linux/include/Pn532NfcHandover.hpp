#pragma once

// Physical NFC handover on Linux through libnfc (PN532 over USB-UART/HSU,
// I2C or SPI -- whatever libnfc's connection string selects).
//
// The PN532 acts as an ISO 14443-4 reader: it SELECTs the AnyShare HCE
// application on a phone in receive mode and reads the receiver's handover
// JSON from the response. Compiled only when ANYSHARE_HAVE_LIBNFC is defined.
#include <stdexcept>
#include <string>
#include <vector>

#include "ByteStream.hpp"
#include "INfcHandover.hpp"

// No usable NFC hardware: caller may fall back to Bluetooth / manual address.
class NfcUnavailableError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Pn532NfcHandover : public INfcHandover {
public:
    // connstring examples: "" (libnfc default/autoscan), "pn532_uart:/dev/ttyUSB0",
    // "pn532_i2c:/dev/i2c-1", "pn532_spi:/dev/spidev0.0".
    explicit Pn532NfcHandover(std::string connstring = "", int waitSeconds = 30);

    // Lets another thread stop the wait for a tap (e.g. a Cancel button).
    void setCancelToken(const CancelToken* cancel) { cancel_ = cancel; }

    ConnectionInfo performHandover() override;
    std::string describe() const override;

    // Lists devices libnfc can see and the chip info of the first one.
    // Never claims success unless the device actually opened.
    static std::string probe(const std::string& connstring);

    // ISO 7816-4 SELECT-by-AID command for the AnyShare HCE service.
    static std::vector<uint8_t> selectAidApdu();

private:
    std::string connstring_;
    int waitSeconds_;
    std::string deviceName_;
    const CancelToken* cancel_ = nullptr;
};
