#pragma once

// Test double for the ESP32 bridge: the master side of a pseudo-terminal.
// Answers HELLO like the real firmware and records every line it receives.
#include <pty.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Fake ESP32 on the master side of a pty: answers HELLO, records every line.
class FakeEsp32 {
public:
    explicit FakeEsp32(bool answers) : answers_(answers) {
        char name[128];
        if (openpty(&master_, &slave_, name, nullptr, nullptr) != 0) throw std::runtime_error("openpty failed");
        termios tio{};
        tcgetattr(slave_, &tio);
        cfmakeraw(&tio);   // no echo before the bridge configures the port
        tcsetattr(slave_, TCSANOW, &tio);
        path_ = name;
        thread_ = std::thread([this] { run(); });
    }
    ~FakeEsp32() {
        unplug();
        if (slave_ >= 0) close(slave_);
    }
    const std::string& path() const { return path_; }

    void unplug() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (master_ >= 0) close(master_);
        master_ = -1;
    }

    // The "reader" reports something, as the firmware would.
    void emitLine(const std::string& line) {
        std::string data = line + "\r\n";
        if (master_ >= 0 && write(master_, data.data(), data.size()) < 0) return;
    }

    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    // Waits until `count` lines arrived (or 2 s passed).
    std::vector<std::string> waitFor(std::size_t count) {
        for (int i = 0; i < 200 && lines().size() < count; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return lines();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

private:
    void run() {
        std::string pending;
        while (!stop_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(master_, &set);
            timeval tv{0, 20000};
            if (select(master_ + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
            char buf[256];
            ssize_t n = read(master_, buf, sizeof(buf));
            if (n <= 0) continue;
            pending.append(buf, static_cast<std::size_t>(n));
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    lines_.push_back(line);
                }
                if (line == "HELLO" && answers_) {
                    const char reply[] = "TAG uid=01 sak=0x08\r\nINFO AnyShare NFC bridge (fake)\r\n";
                    if (write(master_, reply, sizeof(reply) - 1) < 0) return;
                }
            }
        }
    }

    bool answers_;
    int master_ = -1, slave_ = -1;
    std::string path_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> lines_;
};
