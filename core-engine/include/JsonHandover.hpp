#pragma once

#include <string>
#include "ConnectionInfoParser.hpp"
#include "INfcHandover.hpp"

// Concrete strategy from the Android branch: obtains ConnectionInfo from a raw
// JSON payload. The payload's origin (NFC read, file, QR, test) is recorded in
// `source` so diagnostics never claim more than actually happened.
class JsonHandover : public INfcHandover {
public:
    explicit JsonHandover(std::string jsonPayload, std::string source = "JSON payload")
        : jsonPayload_(std::move(jsonPayload)), source_(std::move(source)) {}

    // Throws std::invalid_argument with a clear message (e.g. "Missing required field: ip").
    ConnectionInfo performHandover() override { return anyshare::parseConnectionInfo(jsonPayload_); }

    std::string describe() const override { return source_; }

private:
    std::string jsonPayload_;
    std::string source_;
};
