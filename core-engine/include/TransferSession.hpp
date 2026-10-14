#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ByteStream.hpp"
#include "IProgressObserver.hpp"
#include "Protocol.hpp"

// Sender and receiver halves of ASP/1. Both are blocking and run on the
// caller's worker thread over any IByteStream (TCP, RFCOMM, JNI stream).
namespace anyshare {

// Where outgoing bytes come from: a filesystem path (Linux) or a Java
// InputStream over a content:// URI (Android, via JNI).
class IFileSource {
public:
    virtual ~IFileSource() = default;
    // Returns 0 at end of file; throws std::runtime_error on read errors.
    virtual std::size_t read(char* buffer, std::size_t length) = 0;
};

struct OutgoingFile {
    std::string name;                     // advertised leaf name
    uint64_t size = 0;
    std::string mime = "application/octet-stream";
    std::string expectedCrc;              // optional hex CRC32 from handover metadata
    std::function<std::unique_ptr<IFileSource>()> open;   // reopened on every retry
};

// Throws std::runtime_error if `path` is missing or not a regular file.
OutgoingFile outgoingFileFromPath(const std::string& path, const std::string& mime = "",
                                  const std::string& expectedCrc = "");

enum class TransferStatus {
    Success, Rejected, Cancelled, TransportFailed, ProtocolFailed, LocalFileError, IntegrityFailed,
};
const char* toString(TransferStatus status);

struct SendOptions {
    std::string senderName;
    std::string token;                    // from the receiver's tap payload, if any
    std::size_t chunkSize = 256 * 1024;
    int decisionTimeoutMs = 120000;       // how long the receiver's user may take to accept
};

struct SendResult {
    TransferStatus status = TransferStatus::ProtocolFailed;
    std::size_t filesCompleted = 0;       // acknowledged (checksum-verified) by the receiver
    uint64_t bytesSent = 0;
    std::string message;
    // A new connection may succeed: link dropped or data corrupted in transit.
    bool retryable() const {
        return status == TransferStatus::TransportFailed || status == TransferStatus::IntegrityFailed;
    }
};

SendResult sendFiles(IByteStream& stream, const std::vector<OutgoingFile>& files,
                     const SendOptions& options, IProgressObserver* observer = nullptr,
                     CancelToken* cancel = nullptr);

struct ReceiveOptions {
    std::filesystem::path destinationDir;
    protocol::Limits limits;
    std::size_t chunkSize = 256 * 1024;
    uint64_t reserveBytes = 64ull << 20;  // keep this much disk free
    // Offers carrying this token (read from our tap payload) are auto-accepted.
    std::string expectedToken;
    // Asked for every other offer; may block (e.g. waiting for a UI dialog).
    // If unset, only token-matched offers are accepted.
    std::function<bool(const protocol::Offer& offer)> acceptOffer;
};

struct ReceivedFile {
    std::string offeredName;
    std::string savedPath;
    uint64_t size = 0;
    std::string crc;
};

struct ReceiveResult {
    TransferStatus status = TransferStatus::ProtocolFailed;
    std::string senderName;
    bool tokenMatched = false;
    std::vector<ReceivedFile> files;
    std::string message;
};

ReceiveResult receiveFiles(IByteStream& stream, const ReceiveOptions& options,
                           IProgressObserver* observer = nullptr, CancelToken* cancel = nullptr);

// --- Pull (the sender waits, the receiver connects; see Protocol.hpp) -------------
// Sender side, on a connection it accepted: reads the PULL request and sends `files` only if it
// carries options.token (the token this device published in its tap payload). A wrong or missing
// token ends the session before any file name is revealed (status ProtocolFailed, `pullRefused`).
struct PullSendResult : SendResult {
    bool pullRefused = false;      // the peer did not know our token: keep waiting for the right one
    std::string receiverName;
};
PullSendResult sendFilesOnPull(IByteStream& stream, const std::vector<OutgoingFile>& files,
                               const SendOptions& options, IProgressObserver* observer = nullptr,
                               CancelToken* cancel = nullptr);

// Receiver side, on a connection it opened to a device whose tap payload said role "send".
// The offer is accepted automatically when it carries the same token (the user tapped on purpose).
ReceiveResult receiveFilesByPull(IByteStream& stream, const std::string& token, const std::string& receiverName,
                                 ReceiveOptions options, IProgressObserver* observer = nullptr,
                                 CancelToken* cancel = nullptr);

}  // namespace anyshare
