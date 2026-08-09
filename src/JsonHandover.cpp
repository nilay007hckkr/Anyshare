// ============================================================================
// JsonHandover.cpp — Concrete Strategy implementation (GoF Strategy Pattern)
// ============================================================================
// OOP Principle: POLYMORPHISM
// This file provides the concrete implementation of the INfcHandover
// interface. When NetworkEngine calls performHandover() through the abstract
// interface, this override is dispatched at runtime.
//
// Uses nlohmann/json for lightweight, header-only JSON parsing.
// ============================================================================

#include "JsonHandover.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

// OOP Principle: ENCAPSULATION — the JSON payload is stored privately
// and only accessible through the performHandover() interface method.
JsonHandover::JsonHandover(const std::string& json_payload)
    : json_payload_(json_payload)
{
}

// Strategy Pattern: this method is the interchangeable "algorithm" that
// the NetworkEngine calls polymorphically. Other teammates provide their
// own implementations (LinuxNfcHandover, AndroidNfcHandover) that obtain
// ConnectionInfo from NFC hardware instead of a JSON string.
ConnectionInfo JsonHandover::performHandover() {
    // Parse the raw JSON string.
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_payload_);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            std::string("[JsonHandover] Failed to parse JSON payload: ") + e.what()
        );
    }

    // Extract fields into a ConnectionInfo struct.
    // OOP Principle: ENCAPSULATION — ConnectionInfo bundles all connection
    // details into a single cohesive data-transfer object.
    ConnectionInfo info;

    // Validate required fields and provide clear error messages.
    auto extract_field = [&](const char* field_name, auto& target) {
        if (!j.contains(field_name)) {
            throw std::runtime_error(
                std::string("[JsonHandover] Missing required field: ") + field_name
            );
        }
        target = j[field_name].get<std::decay_t<decltype(target)>>();
    };

    extract_field("ssid",     info.ssid);
    extract_field("psk",      info.psk);
    extract_field("ip",       info.ip);
    extract_field("port",     info.port);
    extract_field("filename", info.filename);
    extract_field("filesize", info.filesize);

    return info;
}
