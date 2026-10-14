#include "TransferSession.hpp"

#include <algorithm>
#include <system_error>

#include "Checksum.hpp"

namespace anyshare {

namespace fs = std::filesystem;
using namespace protocol;

const char* toString(TransferStatus status) {
    switch (status) {
        case TransferStatus::Success: return "success";
        case TransferStatus::Rejected: return "rejected";
        case TransferStatus::Cancelled: return "cancelled";
        case TransferStatus::TransportFailed: return "connection failed";
        case TransferStatus::ProtocolFailed: return "protocol error";
        case TransferStatus::LocalFileError: return "local file error";
        case TransferStatus::IntegrityFailed: return "integrity check failed";
    }
    return "unknown";
}

namespace {

class PathFileSource : public IFileSource {
public:
    explicit PathFileSource(const fs::path& path) : file_(openFile(path, "rb")) {
        if (!file_) throw std::runtime_error("cannot open " + path.u8string());
    }
    ~PathFileSource() override { std::fclose(file_); }
    std::size_t read(char* buffer, std::size_t length) override {
        std::size_t n = std::fread(buffer, 1, length, file_);
        if (n == 0 && std::ferror(file_)) throw std::runtime_error("read error");
        return n;
    }

private:
    std::FILE* file_;
};

// Deduplicated overall percentage + raw byte counts.
class ProgressReporter {
public:
    ProgressReporter(IProgressObserver* observer, uint64_t total) : observer_(observer), total_(total) {}
    void add(uint64_t bytes) {
        done_ += bytes;
        if (!observer_) return;
        observer_->onBytesTransferred(done_, total_);
        int percent = total_ == 0 ? 100 : static_cast<int>((done_ * 100) / total_);
        if (percent != lastPercent_) {
            lastPercent_ = percent;
            observer_->onProgress(percent);
        }
    }
    uint64_t done() const { return done_; }

private:
    IProgressObserver* observer_;
    uint64_t total_;
    uint64_t done_ = 0;
    int lastPercent_ = -1;
};

std::string leafName(const std::string& name) {
    std::size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

void bestEffortCancel(IByteStream& stream) {
    try {
        writeSimple(stream, MsgType::Cancel);
    } catch (...) {
    }
}

// Partially received data lives in a hidden temp file that is deleted unless
// it passes the checksum and is committed.
class TempFile {
public:
    explicit TempFile(const fs::path& dir) : path_(dir / (".anyshare-" + randomHex(8) + ".part")) {
        file_ = openFile(path_, "wb");
        if (!file_) throw std::runtime_error("cannot create file in " + dir.u8string());
    }
    ~TempFile() { discard(); }
    bool write(const char* data, std::size_t n) { return file_ && std::fwrite(data, 1, n, file_) == n; }
    bool close() {
        if (!file_) return false;
        bool ok = std::fflush(file_) == 0;
        ok = (std::fclose(file_) == 0) && ok;
        file_ = nullptr;
        return ok;
    }
    void discard() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
            path_.clear();
        }
    }
    const fs::path& path() const { return path_; }
    void release() { path_.clear(); }

private:
    fs::path path_;
    std::FILE* file_ = nullptr;
};

}  // namespace

OutgoingFile outgoingFileFromPath(const std::string& path, const std::string& mime,
                                  const std::string& expectedCrc) {
    fs::path p(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) throw std::runtime_error("file not found: " + path);
    if (!fs::is_regular_file(p, ec)) throw std::runtime_error("not a regular file: " + path);
    OutgoingFile f;
    f.name = p.filename().u8string();
    f.size = fs::file_size(p);
    f.mime = mime.empty() ? "application/octet-stream" : mime;
    f.expectedCrc = expectedCrc;
    f.open = [p] { return std::make_unique<PathFileSource>(p); };
    return f;
}

// ===========================================================================
// Sender
// ===========================================================================

SendResult sendFiles(IByteStream& stream, const std::vector<OutgoingFile>& files,
                     const SendOptions& options, IProgressObserver* observer, CancelToken* cancel) {
    SendResult result;
    auto fail = [&](TransferStatus status, const std::string& message) {
        result.status = status;
        result.message = message;
        if (observer) {
            observer->onError(message);
            observer->onSessionFinished(false, message);
        }
        return result;
    };
    auto cancelled = [&] { return cancel && cancel->isCancelled(); };

    if (files.empty()) return fail(TransferStatus::LocalFileError, "no files to send");

    Offer offer;
    offer.senderName = options.senderName;
    offer.token = options.token;
    for (const auto& f : files) {
        if (!f.open) return fail(TransferStatus::LocalFileError, "no data source for " + f.name);
        offer.files.push_back({leafName(f.name), f.size, f.mime});
    }
    uint64_t total = offer.totalBytes();
    ProgressReporter progress(observer, total);
    std::vector<char> buffer(std::max<std::size_t>(options.chunkSize, 4096));

    try {
        if (cancelled()) return fail(TransferStatus::Cancelled, "cancelled");
        writeOffer(stream, offer);
        std::string reason;
        int linkTimeout = stream.ioTimeoutMs();
        if (linkTimeout > 0) stream.setIoTimeoutMs(std::max(linkTimeout, options.decisionTimeoutMs));
        OfferStatus status = readResponse(stream, &reason);
        stream.setIoTimeoutMs(linkTimeout);
        if (status != OfferStatus::Accepted) {
            return fail(TransferStatus::Rejected, std::string("receiver declined: ") + toString(status) +
                                                      (reason.empty() ? "" : " (" + reason + ")"));
        }
        if (observer) observer->onSessionStarted(files.size(), total);

        for (std::size_t i = 0; i < files.size(); ++i) {
            const OutgoingFile& file = files[i];
            if (cancelled()) {
                bestEffortCancel(stream);
                return fail(TransferStatus::Cancelled, "cancelled");
            }
            std::unique_ptr<IFileSource> source;
            try {
                source = file.open();
            } catch (const std::exception& e) {
                bestEffortCancel(stream);
                return fail(TransferStatus::LocalFileError, "cannot open " + file.name + ": " + e.what());
            }
            if (observer) observer->onFileStarted(i, offer.files[i].name, file.size);

            writeFileHeader(stream, static_cast<uint32_t>(i), file.size);
            Crc32Calculator crc;
            uint64_t remaining = file.size;
            while (remaining > 0) {
                if (cancelled()) {
                    stream.abort();   // mid-file: the receiver discards the partial file
                    return fail(TransferStatus::Cancelled, "cancelled");
                }
                std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(remaining, buffer.size()));
                std::size_t n;
                try {
                    n = source->read(buffer.data(), want);
                } catch (const std::exception& e) {
                    stream.abort();
                    return fail(TransferStatus::LocalFileError, "read failed for " + file.name + ": " + e.what());
                }
                if (n == 0) {
                    // Framing promised `size` bytes; we cannot keep that promise.
                    stream.abort();
                    return fail(TransferStatus::LocalFileError, file.name + " shrank while being sent");
                }
                crc.update(buffer.data(), n);
                stream.writeAll(buffer.data(), n);
                remaining -= n;
                result.bytesSent += n;
                progress.add(n);
            }
            writeFileTrailer(stream, crc.value());

            if (!file.expectedCrc.empty() && file.expectedCrc != crc.getHexValue() && observer) {
                observer->onError("warning: " + file.name + " does not match the checksum in its metadata (expected " +
                                  file.expectedCrc + ", read " + crc.getHexValue() + ")");
            }

            if (readType(stream) != MsgType::FileAck) throw ProtocolError("expected FILE_ACK");
            uint32_t ackIndex = readU32(stream);
            auto ackStatus = static_cast<AckStatus>(readU8(stream));
            uint32_t ackCrc = readU32(stream);
            if (ackIndex != i) throw ProtocolError("FILE_ACK for wrong file");
            if (ackStatus == AckStatus::ChecksumMismatch || (ackStatus == AckStatus::Ok && ackCrc != crc.value())) {
                bestEffortCancel(stream);
                return fail(TransferStatus::IntegrityFailed,
                            "receiver reported checksum mismatch for " + file.name + " (sent " + crc.getHexValue() +
                                ", received " + Crc32Calculator::toHex(ackCrc) + ")");
            }
            if (ackStatus != AckStatus::Ok) {
                bestEffortCancel(stream);
                return fail(TransferStatus::Rejected, std::string("receiver could not store ") + file.name + ": " +
                                                          toString(ackStatus));
            }
            result.filesCompleted = i + 1;
            if (observer) observer->onFileCompleted(i, offer.files[i].name, "", true);
        }

        writeSimple(stream, MsgType::Done);
        if (readType(stream) != MsgType::Done) throw ProtocolError("expected DONE from receiver");
    } catch (const TransportError& e) {
        if (cancelled()) return fail(TransferStatus::Cancelled, "cancelled");
        return fail(TransferStatus::TransportFailed, std::string("connection lost: ") + e.what());
    } catch (const ProtocolError& e) {
        return fail(TransferStatus::ProtocolFailed, e.what());
    } catch (const std::exception& e) {
        return fail(TransferStatus::LocalFileError, e.what());
    }

    result.status = TransferStatus::Success;
    result.message = "sent " + std::to_string(files.size()) + " file(s), all verified by receiver";
    if (observer) observer->onSessionFinished(true, result.message);
    return result;
}

// ===========================================================================
// Receiver
// ===========================================================================

ReceiveResult receiveFiles(IByteStream& stream, const ReceiveOptions& options,
                           IProgressObserver* observer, CancelToken* cancel) {
    ReceiveResult result;
    auto fail = [&](TransferStatus status, const std::string& message) {
        result.status = status;
        result.message = message;
        if (observer) {
            observer->onError(message);
            observer->onSessionFinished(false, message);
        }
        return result;
    };
    auto cancelled = [&] { return cancel && cancel->isCancelled(); };

    try {
        Offer offer;
        try {
            offer = readOffer(stream, options.limits);
        } catch (const OfferError& e) {
            try {
                writeResponse(stream, e.status, e.what());
            } catch (...) {
            }
            return fail(TransferStatus::ProtocolFailed, std::string("refused offer: ") + e.what());
        }
        result.senderName = offer.senderName;
        result.tokenMatched = !options.expectedToken.empty() && constantTimeEquals(offer.token, options.expectedToken);

        bool accepted = result.tokenMatched || (options.acceptOffer && options.acceptOffer(offer));
        if (cancelled()) accepted = false;
        if (!accepted) {
            writeResponse(stream, OfferStatus::Rejected, cancelled() ? "receiver cancelled" : "declined");
            return fail(cancelled() ? TransferStatus::Cancelled : TransferStatus::Rejected, "offer declined");
        }

        std::error_code ec;
        fs::create_directories(options.destinationDir, ec);
        if (!fs::is_directory(options.destinationDir, ec)) {
            writeResponse(stream, OfferStatus::Rejected, "destination unavailable");
            return fail(TransferStatus::LocalFileError,
                        "destination directory unavailable: " + options.destinationDir.u8string());
        }
        uint64_t total = offer.totalBytes();
        fs::space_info space = fs::space(options.destinationDir, ec);
        if (!ec && space.available < total + options.reserveBytes) {
            writeResponse(stream, OfferStatus::InsufficientSpace, "not enough free space");
            return fail(TransferStatus::LocalFileError, "not enough free space for " + std::to_string(total) + " bytes");
        }
        writeResponse(stream, OfferStatus::Accepted, "");
        if (observer) observer->onSessionStarted(offer.files.size(), total);

        ProgressReporter progress(observer, total);
        std::vector<char> buffer(std::max<std::size_t>(options.chunkSize, 4096));
        std::size_t expected = 0;

        for (;;) {
            MsgType type = readType(stream);
            if (type == MsgType::Done) {
                if (expected != offer.files.size()) throw ProtocolError("DONE before all files were sent");
                writeSimple(stream, MsgType::Done);
                break;
            }
            if (type == MsgType::Cancel) {
                return fail(TransferStatus::Cancelled, "sender cancelled the transfer");
            }
            if (type != MsgType::FileHeader) throw ProtocolError("unexpected message from sender");

            uint32_t index = readU32(stream);
            uint64_t size = readU64(stream);
            if (index != expected || index >= offer.files.size() || size != offer.files[index].size) {
                throw ProtocolError("file header does not match the accepted offer");
            }
            const std::string safeName = sanitizeFilename(offer.files[index].name);
            if (observer) observer->onFileStarted(index, safeName, size);

            TempFile temp(options.destinationDir);
            bool writeOk = true;
            Crc32Calculator crc;
            uint64_t remaining = size;
            while (remaining > 0) {
                if (cancelled()) {
                    stream.abort();
                    return fail(TransferStatus::Cancelled, "cancelled; partial file removed");
                }
                std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(remaining, buffer.size()));
                std::size_t n = stream.readSome(buffer.data(), want);
                if (n == 0) throw TransportError("connection closed mid-file");
                crc.update(buffer.data(), n);
                // Keep draining after a write failure so the stream stays framed.
                writeOk = writeOk && temp.write(buffer.data(), n);
                remaining -= n;
                progress.add(n);
            }
            if (readType(stream) != MsgType::FileTrailer) throw ProtocolError("expected FILE_TRAILER");
            uint32_t senderCrc = readU32(stream);
            writeOk = temp.close() && writeOk;

            if (!writeOk) {
                temp.discard();
                writeFileAck(stream, index, AckStatus::WriteError, crc.value());
                return fail(TransferStatus::LocalFileError, "could not write " + safeName + " (disk full?)");
            }
            if (crc.value() != senderCrc) {
                temp.discard();
                writeFileAck(stream, index, AckStatus::ChecksumMismatch, crc.value());
                return fail(TransferStatus::IntegrityFailed, "checksum mismatch for " + safeName + "; file discarded");
            }
            fs::path finalPath = commitWithoutOverwrite(temp.path(), options.destinationDir, safeName);
            temp.release();
            writeFileAck(stream, index, AckStatus::Ok, crc.value());

            result.files.push_back({offer.files[index].name, finalPath.u8string(), size, crc.getHexValue()});
            if (observer) observer->onFileCompleted(index, safeName, finalPath.u8string(), true);
            ++expected;
        }
    } catch (const TransportError& e) {
        if (cancelled()) return fail(TransferStatus::Cancelled, "cancelled; partial file removed");
        return fail(TransferStatus::TransportFailed, std::string("connection lost: ") + e.what());
    } catch (const ProtocolError& e) {
        return fail(TransferStatus::ProtocolFailed, e.what());
    } catch (const std::exception& e) {
        return fail(TransferStatus::LocalFileError, e.what());
    }

    result.status = TransferStatus::Success;
    result.message = "received " + std::to_string(result.files.size()) + " file(s), all checksums verified";
    if (observer) observer->onSessionFinished(true, result.message);
    return result;
}

// ===========================================================================
// Pull
// ===========================================================================

PullSendResult sendFilesOnPull(IByteStream& stream, const std::vector<OutgoingFile>& files,
                               const SendOptions& options, IProgressObserver* observer, CancelToken* cancel) {
    PullSendResult result;
    auto refuse = [&](TransferStatus status, const std::string& message) {
        result.status = status;
        result.message = message;
        if (observer) observer->onError(message);   // no onSessionFinished: the sender keeps waiting
        return result;
    };
    PullRequest pull;
    try {
        pull = readPull(stream);
    } catch (const TransportError& e) {
        return refuse(TransferStatus::TransportFailed, std::string("connection lost before the PULL request: ") + e.what());
    } catch (const ProtocolError& e) {
        result.pullRefused = true;
        return refuse(TransferStatus::ProtocolFailed, std::string("not a valid PULL request: ") + e.what());
    }
    result.receiverName = pull.receiverName;
    if (options.token.empty() || !constantTimeEquals(pull.token, options.token)) {
        result.pullRefused = true;
        return refuse(TransferStatus::ProtocolFailed, "connection did not present this tap's token; nothing sent");
    }
    SendResult sent = sendFiles(stream, files, options, observer, cancel);
    static_cast<SendResult&>(result) = sent;
    return result;
}

ReceiveResult receiveFilesByPull(IByteStream& stream, const std::string& token, const std::string& receiverName,
                                 ReceiveOptions options, IProgressObserver* observer, CancelToken* cancel) {
    try {
        writePull(stream, PullRequest{token, receiverName});
    } catch (const TransportError& e) {
        ReceiveResult result;
        result.status = TransferStatus::TransportFailed;
        result.message = std::string("connection lost: ") + e.what();
        if (observer) {
            observer->onError(result.message);
            observer->onSessionFinished(false, result.message);
        }
        return result;
    }
    options.expectedToken = token;   // the sender echoes its own token in the OFFER
    return receiveFiles(stream, options, observer, cancel);
}

}  // namespace anyshare
