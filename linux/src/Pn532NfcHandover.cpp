#include "Pn532NfcHandover.hpp"

#include <nfc/nfc.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "ConnectionInfoParser.hpp"
#include "Protocol.hpp"

namespace {

// Adapter: libnfc's C handles owned by smart pointers, so every exit path
// (including exceptions) releases the device and the context.
struct ContextDeleter {
    void operator()(nfc_context* ctx) const { nfc_exit(ctx); }
};
struct DeviceDeleter {
    void operator()(nfc_device* dev) const { nfc_close(dev); }
};
using ContextPtr = std::unique_ptr<nfc_context, ContextDeleter>;
using DevicePtr = std::unique_ptr<nfc_device, DeviceDeleter>;

ContextPtr openContext() {
    nfc_context* ctx = nullptr;
    nfc_init(&ctx);
    if (!ctx) throw NfcUnavailableError("libnfc initialisation failed");
    return ContextPtr(ctx);
}

DevicePtr openDevice(nfc_context* ctx, const std::string& connstring) {
    nfc_device* dev = nfc_open(ctx, connstring.empty() ? nullptr : connstring.c_str());
    if (!dev) {
        throw NfcUnavailableError(
            "no NFC reader could be opened" + (connstring.empty() ? std::string() : " at '" + connstring + "'") +
            " (check wiring, PN532 HSU/I2C/SPI DIP switches, device permissions, /etc/nfc/libnfc.conf)");
    }
    return DevicePtr(dev);
}

std::string hex(const uint8_t* data, std::size_t n) {
    std::string out;
    char b[3];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X", data[i]);
        out += b;
    }
    return out;
}

}  // namespace

Pn532NfcHandover::Pn532NfcHandover(std::string connstring, int waitSeconds)
    : connstring_(std::move(connstring)), waitSeconds_(waitSeconds) {}

std::string Pn532NfcHandover::describe() const {
    return "physical NFC tap (libnfc" + (deviceName_.empty() ? std::string() : ", " + deviceName_) + ")";
}

std::vector<uint8_t> Pn532NfcHandover::selectAidApdu() {
    const std::string aidHex = anyshare::protocol::kHceAidHex;
    std::vector<uint8_t> apdu = {0x00, 0xA4, 0x04, 0x00, static_cast<uint8_t>(aidHex.size() / 2)};
    for (std::size_t i = 0; i + 1 < aidHex.size(); i += 2) {
        apdu.push_back(static_cast<uint8_t>(std::stoi(aidHex.substr(i, 2), nullptr, 16)));
    }
    apdu.push_back(0x00);   // Le: return up to 256 bytes
    return apdu;
}

ConnectionInfo Pn532NfcHandover::performHandover() {
    ContextPtr ctx = openContext();
    DevicePtr dev = openDevice(ctx.get(), connstring_);
    deviceName_ = nfc_device_get_name(dev.get());
    if (nfc_initiator_init(dev.get()) < 0) {
        throw NfcUnavailableError(std::string("NFC reader init failed: ") + nfc_strerror(dev.get()));
    }
    nfc_device_set_property_bool(dev.get(), NP_INFINITE_SELECT, false);

    std::printf("[NFC] %s ready. Hold the receiving phone (AnyShare in receive mode) on the reader...\n",
                deviceName_.c_str());
    const nfc_modulation modulation = {NMT_ISO14443A, NBR_106};
    nfc_target target;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds_);
    for (;;) {
        if (nfc_initiator_select_passive_target(dev.get(), modulation, nullptr, 0, &target) > 0) break;
        if (cancel_ && cancel_->isCancelled()) throw std::runtime_error("waiting for an NFC tap was cancelled");
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("no NFC tap detected within " + std::to_string(waitSeconds_) + " s");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    // SAK bit 6 (0x20) = ISO 14443-4 compliant, required for HCE APDUs.
    if ((target.nti.nai.btSak & 0x20) == 0) {
        throw std::runtime_error("tapped object is a plain tag, not a phone running AnyShare (SAK " +
                                 hex(&target.nti.nai.btSak, 1) + ")");
    }

    std::vector<uint8_t> apdu = selectAidApdu();
    uint8_t response[264];
    int n = nfc_initiator_transceive_bytes(dev.get(), apdu.data(), apdu.size(), response, sizeof(response), 2000);
    if (n < 2) throw std::runtime_error(std::string("APDU exchange failed: ") + nfc_strerror(dev.get()));
    uint16_t sw = static_cast<uint16_t>((response[n - 2] << 8) | response[n - 1]);
    if (sw != 0x9000) {
        throw std::runtime_error("phone did not accept the AnyShare AID (SW " + hex(response + n - 2, 2) +
                                 "); is AnyShare open in receive mode?");
    }
    std::string payload(reinterpret_cast<char*>(response), static_cast<std::size_t>(n - 2));
    std::printf("[NFC] Received %d-byte handover payload from phone.\n", n - 2);
    return anyshare::parseConnectionInfo(payload);
}

std::string Pn532NfcHandover::probe(const std::string& connstring) {
    std::string report;
    ContextPtr ctx = openContext();
    nfc_connstring devices[8];
    std::size_t found = nfc_list_devices(ctx.get(), devices, 8);
    report += "libnfc " + std::string(nfc_version()) + ": " + std::to_string(found) + " device(s) listed\n";
    for (std::size_t i = 0; i < found; ++i) report += "  - " + std::string(devices[i]) + "\n";
    DevicePtr dev = openDevice(ctx.get(), connstring);
    report += "Opened: " + std::string(nfc_device_get_name(dev.get())) + " (" +
              nfc_device_get_connstring(dev.get()) + ")\n";
    if (nfc_initiator_init(dev.get()) < 0) {
        report += "Initiator init FAILED: " + std::string(nfc_strerror(dev.get())) + "\n";
        return report;
    }
    char* info = nullptr;
    if (nfc_device_get_information_about(dev.get(), &info) >= 0 && info) {
        report += info;
        nfc_free(info);
    }
    return report;
}
