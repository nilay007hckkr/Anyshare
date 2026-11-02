#pragma once

// Bluetooth Classic RFCOMM transport for Linux (BlueZ / libbluetooth).
// Compiled only when ANYSHARE_HAVE_BLUEZ is defined.
#include <cstdint>
#include <memory>
#include <string>

#include "NetworkEngine.hpp"
#include "SocketStream.hpp"

namespace anyshare::bt {

// Address of the default local adapter ("AA:BB:CC:DD:EE:FF"); throws TransportError if none.
std::string localAdapterAddress();

// Looks up the RFCOMM channel the peer advertises for `uuid` via SDP.
uint8_t findRfcommChannel(const std::string& address, const std::string& uuid);

// Address of a device already paired with this computer whose name equals `name`
// (case-insensitive), from `bluetoothctl`. "" if none. Instant; no inquiry needed.
std::string pairedDeviceByName(const std::string& name);

// Inquiry scan (~10 s) for a discoverable device whose name equals `name`.
// Returns "" if not found. Android phones are only found while discoverable.
std::string findDeviceByName(const std::string& name, int inquirySeconds = 10);

// Connects to `address`; channel 0 = resolve the AnyShare service via SDP.
std::unique_ptr<SocketStream> connectRfcomm(const std::string& address, uint8_t channel, int timeoutMs);

class RfcommListener {
public:
    // channel 0 = first free channel. Registers an SDP record for the AnyShare
    // UUID so Android's createRfcommSocketToServiceRecord() can find us.
    explicit RfcommListener(uint8_t channel = 0);
    ~RfcommListener();
    RfcommListener(const RfcommListener&) = delete;
    RfcommListener& operator=(const RfcommListener&) = delete;

    uint8_t channel() const { return channel_; }
    bool sdpRegistered() const { return sdpSession_ != nullptr; }
    const std::string& sdpError() const { return sdpError_; }
    std::unique_ptr<SocketStream> accept(int timeoutMs, const CancelToken* cancel = nullptr);

private:
    int fd_ = -1;
    uint8_t channel_ = 0;
    void* sdpSession_ = nullptr;   // sdp_session_t*
    std::string sdpError_;
};

// NetworkEngine factory: "rfcomm" via BlueZ, everything else via TCP.
// An "rfcomm" handover that also carries a Wi-Fi ip/port is tried over Wi-Fi first for at most
// `wifiFirstMs` (much faster), then over Bluetooth: the two devices may not share a network.
// wifiFirstMs = 0: Bluetooth only.
NetworkEngine::StreamFactory makeStreamFactory(int wifiFirstMs = 3000);

}  // namespace anyshare::bt
