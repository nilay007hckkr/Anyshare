#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "ByteStream.hpp"

// AnyShare Protocol v1 (ASP/1) -- transport-independent framing shared by the
// Linux CLI, the Android app (via JNI) and the tests. All integers big-endian.
//
//   Sender -> Receiver  OFFER        "ASP1" u8 version u8 type=1 u32 len  JSON
//   Receiver -> Sender  RESPONSE     u8 type=2 u8 status u16 len  reason(UTF-8)
//   for each file i:
//     Sender -> Receiver  FILE_HEADER  u8 type=3 u32 index u64 size, then exactly `size` raw bytes
//     Sender -> Receiver  FILE_TRAILER u8 type=4 u32 crc32
//     Receiver -> Sender  FILE_ACK     u8 type=5 u32 index u8 status u32 crc32(as received)
//   Sender -> Receiver  DONE  u8 type=6        Receiver -> Sender  DONE u8 type=6
//   Sender -> Receiver  CANCEL u8 type=7   (between files; mid-file cancel = close the link)
//
// OFFER JSON: {"protocol":1,"sender":"..","token":"..","files":[{"name":"..","size":N,"mime":".."}]}
//
// Pull: after a tap on a device that can only *read* NFC (the ESP32 + Si512 module), the phone
// with the files publishes {"role":"send",...,"token"} and waits; the receiver connects to it:
//   Receiver -> Sender  PULL  "ASPP" u8 version u16 len  JSON {"token":"..","receiver":".."}
// The sender checks the token (only the device that read its tap knows it), then the session above
// runs unchanged, starting with the sender's OFFER.
namespace anyshare::protocol {

constexpr uint8_t kVersion = 1;
constexpr char kMagic[4] = {'A', 'S', 'P', '1'};

// Bluetooth RFCOMM service UUID and NFC HCE application ID. The Android app
// (Java) and the Linux tools must use exactly these values.
constexpr const char* kBluetoothServiceUuid = "a9f3c6e2-5b1d-4e8a-9c47-2d6b8e1f0a35";
constexpr const char* kBluetoothServiceName = "AnyShare";
constexpr const char* kHceAidHex = "F0414E59534852"; // proprietary AID: F0 + "ANYSHR"

enum class MsgType : uint8_t {
    Offer = 1, Response = 2, FileHeader = 3, FileTrailer = 4, FileAck = 5, Done = 6, Cancel = 7,
};

enum class OfferStatus : uint8_t {
    Accepted = 0, Rejected = 1, TooLarge = 2, InsufficientSpace = 3,
    BadRequest = 4, Busy = 5, UnsupportedVersion = 6,
};

enum class AckStatus : uint8_t { Ok = 0, ChecksumMismatch = 1, WriteError = 2 };

const char* toString(OfferStatus status);
const char* toString(AckStatus status);

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// An OFFER the receiver must refuse; `status` is sent back to the sender.
class OfferError : public ProtocolError {
public:
    OfferError(OfferStatus s, const std::string& message) : ProtocolError(message), status(s) {}
    OfferStatus status;
};

struct Limits {
    std::size_t maxFiles = 4096;
    uint32_t maxOfferJsonBytes = 1u << 20;          // 1 MiB of metadata
    std::size_t maxNameBytes = 255;
    uint64_t maxTotalBytes = 256ull << 30;          // 256 GiB per session
    uint64_t maxFileBytes = 256ull << 30;
};

struct OfferedFile {
    std::string name;   // leaf name as sent by the peer (NOT yet sanitised)
    uint64_t size = 0;
    std::string mime;
};

struct Offer {
    std::string senderName;
    std::string token;
    std::vector<OfferedFile> files;
    uint64_t totalBytes() const;
};

// --- Big-endian helpers ---------------------------------------------------
void putU16(std::string& out, uint16_t v);
void putU32(std::string& out, uint32_t v);
void putU64(std::string& out, uint64_t v);
uint8_t readU8(IByteStream& s);
uint16_t readU16(IByteStream& s);
uint32_t readU32(IByteStream& s);
uint64_t readU64(IByteStream& s);

// --- Messages -------------------------------------------------------------
std::string encodeOfferJson(const Offer& offer);
Offer decodeOfferJson(const std::string& json, const Limits& limits);   // throws OfferError

void writeOffer(IByteStream& s, const Offer& offer);
// Reads magic + version + OFFER. Throws OfferError on invalid or over-limit input.
Offer readOffer(IByteStream& s, const Limits& limits);

void writeResponse(IByteStream& s, OfferStatus status, const std::string& reason);
OfferStatus readResponse(IByteStream& s, std::string* reason);

struct PullRequest {
    std::string token;          // from the sender's tap payload
    std::string receiverName;   // shown on the sender's screen
};
void writePull(IByteStream& s, const PullRequest& pull);
PullRequest readPull(IByteStream& s);   // throws ProtocolError on anything but a well-formed PULL

void writeFileHeader(IByteStream& s, uint32_t index, uint64_t size);
void writeFileTrailer(IByteStream& s, uint32_t crc);
void writeFileAck(IByteStream& s, uint32_t index, AckStatus status, uint32_t crc);
void writeSimple(IByteStream& s, MsgType type);
MsgType readType(IByteStream& s);

// --- Filesystem safety ----------------------------------------------------
// Turns an untrusted peer-supplied name into a safe single path component:
// strips directories, control characters and reserved characters, rejects
// "." / "..", avoids hidden files, and truncates on a UTF-8 boundary.
std::string sanitizeFilename(const std::string& untrusted, std::size_t maxBytes = 200);

// Cryptographically-seeded random hex string (session tokens, temp names).
std::string randomHex(std::size_t bytes);

// Opens a file by path on every platform (UTF-16 API on Windows).
std::FILE* openFile(const std::filesystem::path& path, const char* mode);

// Moves `tempFile` to `dir/name`, adding " (1)", " (2)", ... so that an
// existing file is never overwritten. Returns the final path.
std::filesystem::path commitWithoutOverwrite(const std::filesystem::path& tempFile,
                                             const std::filesystem::path& dir,
                                             const std::string& safeName);

}  // namespace anyshare::protocol
