#include "Protocol.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace anyshare::protocol {

namespace fs = std::filesystem;

const char* toString(OfferStatus status) {
    switch (status) {
        case OfferStatus::Accepted: return "accepted";
        case OfferStatus::Rejected: return "rejected by receiver";
        case OfferStatus::TooLarge: return "offer exceeds receiver limits";
        case OfferStatus::InsufficientSpace: return "receiver has insufficient storage";
        case OfferStatus::BadRequest: return "malformed offer";
        case OfferStatus::Busy: return "receiver busy";
        case OfferStatus::UnsupportedVersion: return "unsupported protocol version";
    }
    return "unknown status";
}

const char* toString(AckStatus status) {
    switch (status) {
        case AckStatus::Ok: return "ok";
        case AckStatus::ChecksumMismatch: return "checksum mismatch";
        case AckStatus::WriteError: return "receiver write error";
    }
    return "unknown ack";
}

uint64_t Offer::totalBytes() const {
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    return total;
}

// --- Big-endian helpers ---------------------------------------------------

void putU16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

void putU32(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(v >> shift));
}

void putU64(std::string& out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>(v >> shift));
}

uint8_t readU8(IByteStream& s) {
    uint8_t v;
    s.readExact(&v, 1);
    return v;
}

uint16_t readU16(IByteStream& s) {
    uint8_t b[2];
    s.readExact(b, 2);
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

uint32_t readU32(IByteStream& s) {
    uint8_t b[4];
    s.readExact(b, 4);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

uint64_t readU64(IByteStream& s) {
    uint64_t hi = readU32(s);
    uint64_t lo = readU32(s);
    return (hi << 32) | lo;
}

// --- Messages -------------------------------------------------------------

std::string encodeOfferJson(const Offer& offer) {
    nlohmann::json j;
    j["protocol"] = kVersion;
    j["sender"] = offer.senderName;
    if (!offer.token.empty()) j["token"] = offer.token;
    j["files"] = nlohmann::json::array();
    for (const auto& f : offer.files) {
        j["files"].push_back({{"name", f.name}, {"size", f.size}, {"mime", f.mime}});
    }
    // Replace invalid UTF-8 rather than throwing on odd local filenames.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

Offer decodeOfferJson(const std::string& json, const Limits& limits) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        throw OfferError(OfferStatus::BadRequest, std::string("offer is not valid JSON: ") + e.what());
    }
    auto bad = [](const std::string& why) { return OfferError(OfferStatus::BadRequest, why); };

    if (!j.is_object() || !j.contains("files") || !j["files"].is_array()) throw bad("offer has no file list");
    if (j.contains("protocol") && (!j["protocol"].is_number_unsigned() || j["protocol"].get<uint64_t>() != kVersion)) {
        throw OfferError(OfferStatus::UnsupportedVersion, "unsupported protocol version in offer");
    }

    Offer offer;
    if (j.contains("sender")) {
        if (!j["sender"].is_string()) throw bad("sender must be a string");
        offer.senderName = j["sender"].get<std::string>().substr(0, 248);
    }
    if (j.contains("token")) {
        if (!j["token"].is_string() || j["token"].get<std::string>().size() > 64) throw bad("invalid token");
        offer.token = j["token"].get<std::string>();
    }

    const auto& files = j["files"];
    if (files.empty()) throw bad("offer contains no files");
    if (files.size() > limits.maxFiles) {
        throw OfferError(OfferStatus::TooLarge, "offer has too many files (" + std::to_string(files.size()) + ")");
    }

    uint64_t total = 0;
    for (const auto& f : files) {
        if (!f.is_object()) throw bad("file entry must be an object");
        if (!f.contains("name") || !f["name"].is_string()) throw bad("file entry has no name");
        if (!f.contains("size") || !f["size"].is_number_unsigned()) throw bad("file entry has no valid size");
        OfferedFile of;
        of.name = f["name"].get<std::string>();
        of.size = f["size"].get<uint64_t>();
        if (f.contains("mime") && f["mime"].is_string()) of.mime = f["mime"].get<std::string>().substr(0, 255);
        if (of.name.empty() || of.name.size() > limits.maxNameBytes) throw bad("file name empty or too long");
        if (of.size > limits.maxFileBytes) throw OfferError(OfferStatus::TooLarge, "file too large: " + of.name);
        if (total + of.size < total || total + of.size > limits.maxTotalBytes) {
            throw OfferError(OfferStatus::TooLarge, "offer exceeds the receiver's size limit");
        }
        total += of.size;
        offer.files.push_back(std::move(of));
    }
    return offer;
}

void writeOffer(IByteStream& s, const Offer& offer) {
    std::string json = encodeOfferJson(offer);
    std::string frame(kMagic, sizeof(kMagic));
    frame.push_back(static_cast<char>(kVersion));
    frame.push_back(static_cast<char>(MsgType::Offer));
    putU32(frame, static_cast<uint32_t>(json.size()));
    frame += json;
    s.writeAll(frame.data(), frame.size());
    s.flush();
}

Offer readOffer(IByteStream& s, const Limits& limits) {
    char magic[4];
    s.readExact(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        throw OfferError(OfferStatus::BadRequest, "peer is not speaking ASP/1 (bad magic)");
    }
    uint8_t version = readU8(s);
    if (version != kVersion) {
        throw OfferError(OfferStatus::UnsupportedVersion,
                         "peer uses protocol version " + std::to_string(version));
    }
    if (static_cast<MsgType>(readU8(s)) != MsgType::Offer) {
        throw OfferError(OfferStatus::BadRequest, "expected OFFER message");
    }
    uint32_t length = readU32(s);
    if (length > limits.maxOfferJsonBytes) {
        throw OfferError(OfferStatus::TooLarge, "offer metadata too large");
    }
    std::string json(length, '\0');
    s.readExact(json.data(), length);
    return decodeOfferJson(json, limits);
}

void writeResponse(IByteStream& s, OfferStatus status, const std::string& reason) {
    std::string text = reason.substr(0, 1024);
    std::string frame;
    frame.push_back(static_cast<char>(MsgType::Response));
    frame.push_back(static_cast<char>(status));
    putU16(frame, static_cast<uint16_t>(text.size()));
    frame += text;
    s.writeAll(frame.data(), frame.size());
    s.flush();
}

OfferStatus readResponse(IByteStream& s, std::string* reason) {
    if (readType(s) != MsgType::Response) throw ProtocolError("expected RESPONSE from receiver");
    auto status = static_cast<OfferStatus>(readU8(s));
    uint16_t length = readU16(s);
    std::string text(length, '\0');
    s.readExact(text.data(), length);
    if (reason) *reason = text;
    return status;
}

namespace {
constexpr char kPullMagic[4] = {'A', 'S', 'P', 'P'};
constexpr uint16_t kMaxPullJsonBytes = 1024;
}  // namespace

void writePull(IByteStream& s, const PullRequest& pull) {
    nlohmann::json j{{"token", pull.token}, {"receiver", pull.receiverName.substr(0, 248)}};
    std::string json = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::string frame(kPullMagic, sizeof(kPullMagic));
    frame.push_back(static_cast<char>(kVersion));
    putU16(frame, static_cast<uint16_t>(json.size()));
    frame += json;
    s.writeAll(frame.data(), frame.size());
    s.flush();
}

PullRequest readPull(IByteStream& s) {
    char magic[4];
    s.readExact(magic, sizeof(magic));
    if (std::memcmp(magic, kPullMagic, sizeof(magic)) != 0) throw ProtocolError("expected a PULL request");
    uint8_t version = readU8(s);
    if (version != kVersion) throw ProtocolError("peer uses protocol version " + std::to_string(version));
    uint16_t length = readU16(s);
    if (length > kMaxPullJsonBytes) throw ProtocolError("PULL request too large");
    std::string json(length, '\0');
    s.readExact(json.data(), length);
    PullRequest pull;
    try {
        nlohmann::json j = nlohmann::json::parse(json);
        if (!j.is_object() || !j.contains("token") || !j.at("token").is_string()) {
            throw ProtocolError("PULL request without a token");
        }
        pull.token = j.at("token").get<std::string>();
        if (j.contains("receiver") && j.at("receiver").is_string()) pull.receiverName = j.at("receiver").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError(std::string("malformed PULL request: ") + e.what());
    }
    return pull;
}

void writeFileHeader(IByteStream& s, uint32_t index, uint64_t size) {
    std::string frame;
    frame.push_back(static_cast<char>(MsgType::FileHeader));
    putU32(frame, index);
    putU64(frame, size);
    s.writeAll(frame.data(), frame.size());
}

void writeFileTrailer(IByteStream& s, uint32_t crc) {
    std::string frame;
    frame.push_back(static_cast<char>(MsgType::FileTrailer));
    putU32(frame, crc);
    s.writeAll(frame.data(), frame.size());
    s.flush();
}

void writeFileAck(IByteStream& s, uint32_t index, AckStatus status, uint32_t crc) {
    std::string frame;
    frame.push_back(static_cast<char>(MsgType::FileAck));
    putU32(frame, index);
    frame.push_back(static_cast<char>(status));
    putU32(frame, crc);
    s.writeAll(frame.data(), frame.size());
    s.flush();
}

void writeSimple(IByteStream& s, MsgType type) {
    char b = static_cast<char>(type);
    s.writeAll(&b, 1);
    s.flush();
}

MsgType readType(IByteStream& s) {
    return static_cast<MsgType>(readU8(s));
}

// --- Filesystem safety ----------------------------------------------------

namespace {

// Length of a valid UTF-8 sequence starting at s[i], or 0 if invalid.
std::size_t utf8SequenceLength(const std::string& s, std::size_t i) {
    auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
    }
    if (len == 2 && c < 0xC2) return 0;   // overlong encoding
    return len;
}

std::string truncateUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}

bool isWindowsReservedName(const std::string& name) {
    std::string stem = name.substr(0, name.find('.'));
    for (auto& ch : stem) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                     "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                     "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : reserved) {
        if (stem == r) return true;
    }
    return false;
}

}  // namespace

std::string sanitizeFilename(const std::string& untrusted, std::size_t maxBytes) {
    // 1. Keep only the last path component (both separators).
    std::string name = untrusted;
    std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    // 2. Drop invalid UTF-8, control and reserved characters.
    std::string clean;
    for (std::size_t i = 0; i < name.size();) {
        std::size_t len = utf8SequenceLength(name, i);
        if (len == 0) { clean.push_back('_'); ++i; continue; }
        if (len == 1) {
            char c = name[i];
            auto uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc == 0x7F || std::strchr("<>:\"|?*", c) != nullptr) {
                clean.push_back('_');
            } else {
                clean.push_back(c);
            }
        } else {
            clean.append(name, i, len);
        }
        i += len;
    }

    // 3. Trim surrounding spaces and trailing dots; neutralise leading dots
    //    (no hidden files, no "." / "..").
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    std::size_t start = clean.find_first_not_of(' ');
    clean = (start == std::string::npos) ? std::string() : clean.substr(start);
    if (!clean.empty() && clean.front() == '.') clean.front() = '_';
    if (clean.empty()) clean = "file";
    if (isWindowsReservedName(clean)) clean = "_" + clean;

    // 4. Length limit, keeping a short extension when possible.
    if (clean.size() > maxBytes) {
        std::size_t dot = clean.find_last_of('.');
        std::string ext = (dot != std::string::npos && clean.size() - dot <= 16 && maxBytes > 32)
                              ? clean.substr(dot) : "";
        clean = truncateUtf8(clean.substr(0, clean.size() - ext.size()), maxBytes - ext.size()) + ext;
    }
    return clean;
}

std::string randomHex(std::size_t bytes) {
    // std::random_device reads the OS CSPRNG on Linux/Android (getrandom /
    // /dev/urandom) and on MinGW-w64 GCC >= 9 (rand_s).
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes; ++i) {
        unsigned v = rd() & 0xFF;
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 0xF]);
    }
    return out;
}

std::FILE* openFile(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

fs::path commitWithoutOverwrite(const fs::path& tempFile, const fs::path& dir, const std::string& safeName) {
    std::size_t dot = safeName.find_last_of('.');
    std::string stem = (dot == std::string::npos || dot == 0) ? safeName : safeName.substr(0, dot);
    std::string ext = (dot == std::string::npos || dot == 0) ? "" : safeName.substr(dot);

    for (int n = 0; n < 10000; ++n) {
        std::string candidateName = n == 0 ? safeName : stem + " (" + std::to_string(n) + ")" + ext;
        fs::path candidate = dir / fs::u8path(candidateName);
#ifdef _WIN32
        if (MoveFileExW(tempFile.c_str(), candidate.c_str(), 0)) return candidate;   // never replaces
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) continue;
        throw std::system_error(static_cast<int>(err), std::system_category(), "cannot move received file");
#else
        // link() is atomic and fails with EEXIST instead of overwriting.
        if (::link(tempFile.c_str(), candidate.c_str()) == 0) {
            ::unlink(tempFile.c_str());
            return candidate;
        }
        if (errno == EEXIST) continue;
        // Filesystems without hard links (FAT, some Android FUSE mounts):
        // check-then-rename. Only another local writer racing on the same
        // name could defeat this, which the receiver itself never does.
        std::error_code ec;
        if (fs::exists(candidate, ec)) continue;
        fs::rename(tempFile, candidate);
        return candidate;
#endif
    }
    throw std::runtime_error("too many files named " + safeName);
}

}  // namespace anyshare::protocol
