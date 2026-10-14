#pragma once

#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "INfcHandover.hpp"

// Address typed in by the user or picked from a paired-device list. No NFC involved.
class ManualHandover : public INfcHandover {
public:
    ManualHandover(ConnectionInfo info, std::string how) : info_(std::move(info)), how_(std::move(how)) {}
    ConnectionInfo performHandover() override { return info_; }
    std::string describe() const override { return how_; }

private:
    ConnectionInfo info_;
    std::string how_;
};

// Tries `primary` (e.g. physical NFC); if it throws an exception for which
// `shouldFallBack` returns true (e.g. NFC hardware missing), logs the reason
// and uses `fallback` instead. describe() always names the strategy that
// actually produced the connection info.
class FallbackHandover : public INfcHandover {
public:
    using Predicate = std::function<bool(const std::exception&)>;

    FallbackHandover(std::unique_ptr<INfcHandover> primary, std::unique_ptr<INfcHandover> fallback,
                     Predicate shouldFallBack = [](const std::exception&) { return true; })
        : primary_(std::move(primary)), fallback_(std::move(fallback)), shouldFallBack_(std::move(shouldFallBack)) {}

    ConnectionInfo performHandover() override {
        try {
            used_ = primary_.get();
            return primary_->performHandover();
        } catch (const std::exception& e) {
            if (!fallback_ || !shouldFallBack_(e)) throw;
            fallbackReason_ = e.what();
            std::cerr << "[Handover] " << primary_->describe() << " unavailable: " << e.what()
                      << "\n[Handover] Falling back to " << fallback_->describe() << std::endl;
            used_ = fallback_.get();
            return fallback_->performHandover();
        }
    }

    std::string describe() const override {
        if (used_ == fallback_.get() && used_) {
            return fallback_->describe() + " (fallback; " + primary_->describe() + " failed: " + fallbackReason_ + ")";
        }
        return primary_->describe();
    }

private:
    std::unique_ptr<INfcHandover> primary_;
    std::unique_ptr<INfcHandover> fallback_;
    Predicate shouldFallBack_;
    INfcHandover* used_ = nullptr;
    std::string fallbackReason_;
};
