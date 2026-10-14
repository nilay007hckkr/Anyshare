#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

// Raised by any transport when the peer disconnects, a timeout expires, or
// the OS reports an I/O error. The transfer layer treats it as retryable.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Transport-independent, blocking, bidirectional byte stream. Implementations:
//   SocketStream  (POSIX TCP / BlueZ RFCOMM file descriptor, Linux + Android NDK)
//   JniByteStream (java.io.InputStream/OutputStream, e.g. Android BluetoothSocket)
// Blocking I/O on a dedicated worker thread keeps the protocol code linear and
// identical on every platform; throughput is bounded by the link, not the API.
class IByteStream {
public:
    virtual ~IByteStream() = default;

    // Reads at least 1 and at most `length` bytes. Returns 0 on orderly EOF.
    // Throws TransportError on failure or timeout.
    virtual std::size_t readSome(void* buffer, std::size_t length) = 0;

    // Writes all `length` bytes or throws TransportError.
    virtual void writeAll(const void* buffer, std::size_t length) = 0;

    virtual void flush() {}

    // Unblocks pending I/O from another thread (used for cancellation).
    virtual void abort() {}

    // Idle timeout for a single read/write, if the transport supports one
    // (0 = none). Lets the sender wait longer while a human decides on an offer.
    virtual int ioTimeoutMs() const { return 0; }
    virtual void setIoTimeoutMs(int timeoutMs) { (void)timeoutMs; }

    // Human-readable transport description for logs/diagnostics, e.g. "tcp 127.0.0.1:8080".
    virtual std::string describe() const = 0;

    void readExact(void* buffer, std::size_t length) {
        auto* out = static_cast<unsigned char*>(buffer);
        while (length > 0) {
            std::size_t n = readSome(out, length);
            if (n == 0) {
                throw TransportError("connection closed by peer");
            }
            out += n;
            length -= n;
        }
    }
};

// Thread-safe cancellation flag. The owner of a blocking stream registers an
// abort hook so that cancel() also interrupts a read/write in progress.
class CancelToken {
public:
    void cancel() {
        cancelled_.store(true);
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = onCancel_;
        }
        if (hook) hook();
    }

    bool isCancelled() const { return cancelled_.load(); }

    void reset() { cancelled_.store(false); }

    void setOnCancel(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        onCancel_ = std::move(hook);
    }

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::function<void()> onCancel_;
};
