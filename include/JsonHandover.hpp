#pragma once
// ============================================================================
// JsonHandover.hpp — Concrete Strategy (GoF Strategy Pattern)
// ============================================================================
// OOP Principle: INHERITANCE + POLYMORPHISM
// JsonHandover inherits from INfcHandover and provides a concrete
// implementation of performHandover(). It parses a raw JSON string
// into a ConnectionInfo struct using nlohmann/json.
//
// This is the "test/demo" strategy. Teammates will write their own:
//   - LinuxNfcHandover  (Member 2, PN532 hardware via libnfc)
//   - AndroidNfcHandover (Member 3, Android NFC APIs via JNI)
// All three are interchangeable via the INfcHandover interface.
// ============================================================================

#include <string>
#include "INfcHandover.hpp"

class JsonHandover : public INfcHandover {
public:
    // OOP Principle: ENCAPSULATION
    // The raw JSON string is stored privately. The only way to extract
    // ConnectionInfo from it is through the performHandover() interface.
    explicit JsonHandover(const std::string& json_payload);

    // Strategy method: parses the JSON payload and returns ConnectionInfo.
    // Throws std::runtime_error if the JSON is malformed or missing fields.
    // OOP Principle: POLYMORPHISM — this override is dispatched at runtime
    // when called through an INfcHandover* or INfcHandover& reference.
    ConnectionInfo performHandover() override;

private:
    std::string json_payload_;
};
