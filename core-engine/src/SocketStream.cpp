#include "SocketStream.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace anyshare {

namespace {

std::string errnoText(const std::string& what, int err) {
    return what + ": " + std::strerror(err);
}

void setNonBlocking(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;   // never raise SIGPIPE on a dead peer
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

SocketStream::SocketStream(int fd, std::string description, int ioTimeoutMs)
    : fd_(fd), description_(std::move(description)), ioTimeoutMs_(ioTimeoutMs) {}

SocketStream::~SocketStream() {
    if (fd_ >= 0) ::close(fd_);
}

void SocketStream::abort() {
    aborted_.store(true);
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);   // wakes a thread blocked in poll/recv/send
}

void SocketStream::waitFor(short events) {
    if (ioTimeoutMs_ <= 0) return;
    pollfd p{fd_, events, 0};
    for (;;) {
        int r = ::poll(&p, 1, ioTimeoutMs_);
        if (r > 0) return;
        if (r == 0) throw TransportError("no data for " + std::to_string(ioTimeoutMs_ / 1000) + "s (link timeout)");
        if (errno != EINTR) throw TransportError(errnoText("poll", errno));
    }
}

std::size_t SocketStream::readSome(void* buffer, std::size_t length) {
    for (;;) {
        if (aborted_) throw TransportError("aborted");
        waitFor(POLLIN);
        ssize_t n = ::recv(fd_, buffer, length, 0);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno == EINTR) continue;
        throw TransportError(errnoText("recv", errno));
    }
}

void SocketStream::writeAll(const void* buffer, std::size_t length) {
    auto* p = static_cast<const char*>(buffer);
    while (length > 0) {
        if (aborted_) throw TransportError("aborted");
        waitFor(POLLOUT);
        ssize_t n = ::send(fd_, p, length, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw TransportError(errnoText("send", errno));
        }
        p += n;
        length -= static_cast<std::size_t>(n);
    }
}

void finishConnect(int fd, int timeoutMs, const std::string& what) {
    pollfd p{fd, POLLOUT, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    if (r == 0) throw TransportError(what + ": connect timed out");
    if (r < 0) throw TransportError(errnoText(what + ": poll", errno));
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) throw TransportError(errnoText(what, err));
}

std::unique_ptr<SocketStream> connectTcp(const std::string& host, uint16_t port, int timeoutMs, int ioTimeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    int gai = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (gai != 0) throw TransportError("cannot resolve " + host + ": " + gai_strerror(gai));

    std::string lastError = "no addresses";
    for (addrinfo* ai = results; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        setNonBlocking(fd, true);
        try {
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
                if (errno != EINPROGRESS) throw TransportError(errnoText("connect", errno));
                finishConnect(fd, timeoutMs, "connect to " + host + ":" + std::to_string(port));
            }
        } catch (const TransportError& e) {
            lastError = e.what();
            ::close(fd);
            continue;
        }
        setNonBlocking(fd, false);
        int one = 1;
        // Small control frames (trailer, ACK) must not wait for Nagle/delayed-ACK.
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::freeaddrinfo(results);
        return std::make_unique<SocketStream>(fd, "tcp " + host + ":" + std::to_string(port), ioTimeoutMs);
    }
    ::freeaddrinfo(results);
    throw TransportError(lastError);
}

int acceptWithTimeout(int listenFd, int timeoutMs, const CancelToken* cancel, std::string* peer) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        if (cancel && cancel->isCancelled()) return -1;
        int slice = 250;
        if (timeoutMs >= 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) return -1;
            slice = static_cast<int>(std::min<long long>(slice, left));
        }
        pollfd p{listenFd, POLLIN, 0};
        int r = ::poll(&p, 1, slice);
        if (r < 0 && errno != EINTR) throw TransportError(errnoText("poll", errno));
        if (r <= 0) continue;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
            throw TransportError(errnoText("accept", errno));
        }
        if (peer && (addr.ss_family == AF_INET || addr.ss_family == AF_INET6)) {
            char host[INET6_ADDRSTRLEN] = "?";
            uint16_t port = 0;
            if (addr.ss_family == AF_INET) {
                auto* in = reinterpret_cast<sockaddr_in*>(&addr);
                inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
                port = ntohs(in->sin_port);
            } else {
                auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
                inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
                port = ntohs(in6->sin6_port);
            }
            *peer = std::string(host) + ":" + std::to_string(port);
        }
        return fd;
    }
}

TcpListener::TcpListener(const std::string& bindAddress, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) throw TransportError(errnoText("socket", errno));
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddress.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        ::close(fd_);
        throw TransportError("invalid bind address: " + bindAddress);
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd_, 4) != 0) {
        int err = errno;
        ::close(fd_);
        throw TransportError(errnoText("listen on port " + std::to_string(port), err));
    }
    socklen_t len = sizeof(addr);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::unique_ptr<SocketStream> TcpListener::accept(int timeoutMs, const CancelToken* cancel) {
    std::string peer;
    int fd = acceptWithTimeout(fd_, timeoutMs, cancel, &peer);
    if (fd < 0) return nullptr;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return std::make_unique<SocketStream>(fd, "tcp " + peer, 30000);
}

}  // namespace anyshare
