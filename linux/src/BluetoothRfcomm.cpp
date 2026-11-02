#include "BluetoothRfcomm.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "ConnectionInfoParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Protocol.hpp"

namespace anyshare::bt {

namespace {

// BDADDR_ANY / BDADDR_LOCAL are C compound literals that C++ rejects.
const bdaddr_t kAny = {{0, 0, 0, 0, 0, 0}};
const bdaddr_t kLocal = {{0, 0, 0, 0xff, 0xff, 0xff}};

bdaddr_t parseAddress(const std::string& text) {
    bdaddr_t addr{};
    if (str2ba(text.c_str(), &addr) != 0) throw TransportError("invalid Bluetooth address: " + text);
    return addr;
}

void uuidFromString(const std::string& text, uuid_t* out) {
    uint8_t bytes[16];
    std::size_t n = 0;
    for (std::size_t i = 0; i + 1 < text.size() && n < 16;) {
        if (text[i] == '-') { ++i; continue; }
        bytes[n++] = static_cast<uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16));
        i += 2;
    }
    if (n != 16) throw std::invalid_argument("bad UUID " + text);
    uint128_t value;
    std::memcpy(&value, bytes, 16);   // network byte order, as BlueZ expects
    sdp_uuid128_create(out, &value);
}

}  // namespace

std::string localAdapterAddress() {
    int devId = hci_get_route(nullptr);
    if (devId < 0) throw TransportError("no Bluetooth adapter found");
    bdaddr_t addr{};
    if (hci_devba(devId, &addr) < 0) throw TransportError(std::string("cannot read adapter address: ") + std::strerror(errno));
    char text[19];
    ba2str(&addr, text);
    return text;
}

uint8_t findRfcommChannel(const std::string& address, const std::string& uuid) {
    bdaddr_t target = parseAddress(address);
    sdp_session_t* session = sdp_connect(&kAny, &target, SDP_RETRY_IF_BUSY);
    if (!session) throw TransportError("SDP connect to " + address + " failed: " + std::strerror(errno));

    uuid_t service;
    uuidFromString(uuid, &service);
    sdp_list_t* search = sdp_list_append(nullptr, &service);
    uint32_t range = 0x0000ffff;
    sdp_list_t* attrs = sdp_list_append(nullptr, &range);
    sdp_list_t* response = nullptr;
    int rc = sdp_service_search_attr_req(session, search, SDP_ATTR_REQ_RANGE, attrs, &response);

    int channel = 0;
    for (sdp_list_t* r = response; r; r = r->next) {
        auto* rec = static_cast<sdp_record_t*>(r->data);
        sdp_list_t* protos = nullptr;
        if (channel == 0 && sdp_get_access_protos(rec, &protos) == 0) {
            channel = sdp_get_proto_port(protos, RFCOMM_UUID);
            sdp_list_foreach(protos, reinterpret_cast<sdp_list_func_t>(sdp_list_free), nullptr);
            sdp_list_free(protos, nullptr);
        }
        sdp_record_free(rec);
    }
    sdp_list_free(response, nullptr);
    sdp_list_free(search, nullptr);
    sdp_list_free(attrs, nullptr);
    sdp_close(session);

    if (rc < 0 || channel <= 0) {
        throw TransportError("AnyShare service not found on " + address + " (is the receiver in receive mode?)");
    }
    return static_cast<uint8_t>(channel);
}

std::string findDeviceByName(const std::string& name, int inquirySeconds) {
    int devId = hci_get_route(nullptr);
    if (devId < 0) throw TransportError("no Bluetooth adapter found");
    int sock = hci_open_dev(devId);
    if (sock < 0) throw TransportError(std::string("cannot open adapter: ") + std::strerror(errno));

    inquiry_info* info = nullptr;
    int length = std::max(1, inquirySeconds * 100 / 128);   // units of 1.28 s
    int found = hci_inquiry(devId, length, 255, nullptr, &info, IREQ_CACHE_FLUSH);
    std::string result;
    for (int i = 0; i < found && result.empty(); ++i) {
        char remote[248] = {0};
        if (hci_read_remote_name(sock, &info[i].bdaddr, sizeof(remote), remote, 0) == 0 && name == remote) {
            char text[19];
            ba2str(&info[i].bdaddr, text);
            result = text;
        }
    }
    bt_free(info);
    close(sock);
    return result;
}

std::unique_ptr<SocketStream> connectRfcomm(const std::string& address, uint8_t channel, int timeoutMs) {
    if (channel == 0) channel = findRfcommChannel(address, protocol::kBluetoothServiceUuid);

    int fd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (fd < 0) throw TransportError(std::string("RFCOMM socket: ") + std::strerror(errno));
    sockaddr_rc addr{};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_bdaddr = parseAddress(address);
    addr.rc_channel = channel;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    try {
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            if (errno != EINPROGRESS) throw TransportError(std::string("RFCOMM connect: ") + std::strerror(errno));
            finishConnect(fd, timeoutMs, "RFCOMM connect to " + address);
        }
    } catch (...) {
        close(fd);
        throw;
    }
    fcntl(fd, F_SETFL, flags);
    return std::make_unique<SocketStream>(fd, "bluetooth-rfcomm " + address + " ch " + std::to_string(channel));
}

RfcommListener::RfcommListener(uint8_t channel) {
    fd_ = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (fd_ < 0) throw TransportError(std::string("RFCOMM socket: ") + std::strerror(errno));

    // Encrypted + authenticated link: unpaired peers must pair first.
    int lm = RFCOMM_LM_AUTH | RFCOMM_LM_ENCRYPT;
    setsockopt(fd_, SOL_RFCOMM, RFCOMM_LM, &lm, sizeof(lm));

    bool bound = false;
    for (uint8_t ch = channel ? channel : 1; ch <= (channel ? channel : 30) && !bound; ++ch) {
        sockaddr_rc addr{};
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_bdaddr = kAny;
        addr.rc_channel = ch;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            channel_ = ch;
            bound = true;
        }
    }
    if (!bound || listen(fd_, 1) != 0) {
        int err = errno;
        close(fd_);
        throw TransportError(std::string("RFCOMM listen: ") + std::strerror(err));
    }

    // SDP record so Android can connect by UUID. Needs bluetoothd's legacy SDP
    // socket (run bluetoothd with --compat); without it peers must be given the
    // channel explicitly.
    auto* session = sdp_connect(&kAny, &kLocal, SDP_RETRY_IF_BUSY);
    if (!session) {
        sdpError_ = std::string("cannot reach local SDP server (start bluetoothd with --compat): ") + std::strerror(errno);
        return;
    }
    uuid_t service, root, l2cap, rfcomm;
    uuidFromString(protocol::kBluetoothServiceUuid, &service);
    sdp_record_t* record = sdp_record_alloc();
    sdp_set_service_id(record, service);
    sdp_list_t* classes = sdp_list_append(nullptr, &service);
    sdp_set_service_classes(record, classes);
    sdp_uuid16_create(&root, PUBLIC_BROWSE_GROUP);
    sdp_list_t* rootList = sdp_list_append(nullptr, &root);
    sdp_set_browse_groups(record, rootList);
    sdp_uuid16_create(&l2cap, L2CAP_UUID);
    sdp_list_t* l2capList = sdp_list_append(nullptr, &l2cap);
    sdp_list_t* protoList = sdp_list_append(nullptr, l2capList);
    sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
    sdp_data_t* chan = sdp_data_alloc(SDP_UINT8, &channel_);
    sdp_list_t* rfcommList = sdp_list_append(nullptr, &rfcomm);
    sdp_list_append(rfcommList, chan);
    sdp_list_append(protoList, rfcommList);
    sdp_list_t* access = sdp_list_append(nullptr, protoList);
    sdp_set_access_protos(record, access);
    sdp_set_info_attr(record, protocol::kBluetoothServiceName, "AnyShare", "ASP/1 file transfer");

    if (sdp_record_register(session, record, 0) < 0) {
        sdpError_ = std::string("SDP registration failed: ") + std::strerror(errno);
        sdp_close(session);
        session = nullptr;
    }
    sdp_data_free(chan);
    sdp_list_free(l2capList, nullptr);
    sdp_list_free(rfcommList, nullptr);
    sdp_list_free(rootList, nullptr);
    sdp_list_free(access, nullptr);
    sdp_list_free(protoList, nullptr);
    sdp_list_free(classes, nullptr);
    sdpSession_ = session;   // closing the session unregisters the record
}

RfcommListener::~RfcommListener() {
    if (sdpSession_) sdp_close(static_cast<sdp_session_t*>(sdpSession_));
    if (fd_ >= 0) close(fd_);
}

std::unique_ptr<SocketStream> RfcommListener::accept(int timeoutMs, const CancelToken* cancel) {
    int fd = acceptWithTimeout(fd_, timeoutMs, cancel, nullptr);
    if (fd < 0) return nullptr;
    sockaddr_rc peer{};
    socklen_t len = sizeof(peer);
    getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len);
    char text[19];
    ba2str(&peer.rc_bdaddr, text);
    return std::make_unique<SocketStream>(fd, std::string("bluetooth-rfcomm ") + text);
}

std::string pairedDeviceByName(const std::string& name) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    // BlueZ >= 5.65 has "devices Paired"; older versions "paired-devices".
    for (const char* cmd : {"bluetoothctl devices Paired 2>/dev/null", "bluetoothctl paired-devices 2>/dev/null"}) {
        FILE* out = popen(cmd, "r");
        if (!out) continue;
        char line[512];
        std::string found;
        while (fgets(line, sizeof(line), out)) {   // "Device AA:BB:CC:DD:EE:FF Lakshya's M34"
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.rfind("Device ", 0) != 0 || l.size() < 25) continue;
            std::string address = l.substr(7, 17), deviceName = l.size() > 25 ? l.substr(25) : "";
            if (found.empty() && isValidBluetoothAddress(address) && lower(deviceName) == lower(name)) found = address;
        }
        pclose(out);
        if (!found.empty()) return found;
    }
    return "";
}

NetworkEngine::StreamFactory makeStreamFactory(int wifiFirstMs) {
    return [wifiFirstMs](const ConnectionInfo& info, int timeoutMs) -> std::unique_ptr<IByteStream> {
        if (info.transport == "rfcomm") {
            std::string wifiProblem;
            if (wifiFirstMs > 0 && !info.ip.empty() && info.port != 0) {
                try {
                    return connectTcp(info.ip, info.port, std::min(timeoutMs, wifiFirstMs));
                } catch (const TransportError& e) {
                    wifiProblem = std::string("Wi-Fi ") + e.what() + "; ";
                }
            }
            std::string address = info.btAddress;
            if (address.empty()) {
                // Android cannot publish its own MAC, only its name: a paired phone is found
                // instantly; otherwise an inquiry finds it only while it is discoverable.
                address = pairedDeviceByName(info.deviceName);
                if (address.empty()) address = findDeviceByName(info.deviceName);
                if (address.empty()) {
                    throw TransportError(wifiProblem + "Bluetooth device '" + info.deviceName +
                                         "' is not paired with this computer (pair it once in Bluetooth settings)");
                }
            }
            try {
                return connectRfcomm(address, info.rfcommChannel, timeoutMs);
            } catch (const TransportError& e) {
                throw TransportError(wifiProblem + "Bluetooth " + e.what());
            }
        }
        if (info.ip.empty() || info.port == 0) throw TransportError("no TCP address in connection info");
        return connectTcp(info.ip, info.port, timeoutMs);
    };
}

}  // namespace anyshare::bt
