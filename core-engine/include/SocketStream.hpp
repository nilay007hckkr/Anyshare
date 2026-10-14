#pragma once

// POSIX socket transport (Linux, Android NDK). Not built on Windows.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ByteStream.hpp"

namespace anyshare {

// Wraps any connected stream socket: TCP, or a BlueZ RFCOMM socket.
class SocketStream : public IByteStream {
public:
    // Takes ownership of `fd`. `ioTimeoutMs` bounds how long a single read or
    // write may stall before the link is considered dead (0 = no limit).
    SocketStream(int fd, std::string description, int ioTimeoutMs = 30000);
    ~SocketStream() override;
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    std::size_t readSome(void* buffer, std::size_t length) override;
    void writeAll(const void* buffer, std::size_t length) override;
    void abort() override;
    int ioTimeoutMs() const override { return ioTimeoutMs_; }
    void setIoTimeoutMs(int timeoutMs) override { ioTimeoutMs_ = timeoutMs; }
    std::string describe() const override { return description_; }
    int fd() const { return fd_; }

private:
    void waitFor(short events);
    int fd_;
    std::string description_;
    int ioTimeoutMs_;
    std::atomic<bool> aborted_{false};
};

// Connects with a deadline (non-blocking connect + poll). Throws TransportError.
std::unique_ptr<SocketStream> connectTcp(const std::string& host, uint16_t port, int timeoutMs,
                                         int ioTimeoutMs = 30000);

// Completes a non-blocking connect() on `fd` within `timeoutMs`; throws TransportError.
// Shared by TCP and RFCOMM.
void finishConnect(int fd, int timeoutMs, const std::string& what);

class TcpListener {
public:
    // port 0 picks a free port. bindAddress "" = all interfaces.
    TcpListener(const std::string& bindAddress, uint16_t port);
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    uint16_t port() const { return port_; }
    // Waits up to timeoutMs (-1 = forever) for a client. Returns nullptr on
    // timeout or cancellation. Throws TransportError on socket errors.
    std::unique_ptr<SocketStream> accept(int timeoutMs, const CancelToken* cancel = nullptr);
    void close();

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

// Polls a listening socket with short slices so cancellation is honoured.
// Returns the accepted fd, or -1 on timeout/cancel. Shared with RFCOMM.
int acceptWithTimeout(int listenFd, int timeoutMs, const CancelToken* cancel, std::string* peer);

}  // namespace anyshare
