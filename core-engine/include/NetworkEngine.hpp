#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ByteStream.hpp"
#include "INfcHandover.hpp"
#include "IProgressObserver.hpp"
#include "TransferSession.hpp"

// Sender-side orchestration: handover -> connect (retry every 2 s for up to
// 60 s) -> ASP/1 session -> on a dropped link, reconnect and resend only the
// files the receiver has not yet acknowledged (up to 3 times).
//
// The public API of the original Boost.Asio engine is preserved
// (addObserver / connectToHost / startTransfer). The engine is now blocking
// and transport-agnostic: no Boost dependency, the same code path is used for
// TCP and Bluetooth RFCOMM, and the receiver -- not the sender -- verifies
// every checksum.
class NetworkEngine {
public:
    // Opens a connected stream to the receiver described by `info` or throws TransportError.
    using StreamFactory = std::function<std::unique_ptr<IByteStream>(const ConnectionInfo& info, int timeoutMs)>;

    static constexpr int RETRY_DELAY_SECONDS = 2;
    static constexpr int MAX_CONNECT_TIMEOUT_SECONDS = 60;
    static constexpr int MAX_FILE_RETRIES = 3;

    NetworkEngine();
    ~NetworkEngine();
    NetworkEngine(const NetworkEngine&) = delete;
    NetworkEngine& operator=(const NetworkEngine&) = delete;

    void addObserver(IProgressObserver* observer);

    // Sends info.files to the receiver at info. Blocks until done.
    void connectToHost(const ConnectionInfo& info);

    // Runs the handover strategy, then connectToHost().
    void startTransfer(std::unique_ptr<INfcHandover> handoverStrategy);

    // Thread-safe; interrupts a connect wait or a transfer in progress.
    void cancel();

    // Default factory handles "tcp"; Linux registers an RFCOMM factory.
    void setStreamFactory(StreamFactory factory) { streamFactory_ = std::move(factory); }
    void setSenderName(std::string name) { senderName_ = std::move(name); }
    void setConnectTimeoutSeconds(int seconds) { connectTimeoutSeconds_ = seconds; }
    void setRetryDelaySeconds(int seconds) { retryDelaySeconds_ = seconds; }

    bool lastTransferSucceeded() const { return lastSucceeded_; }
    const std::string& lastMessage() const { return lastMessage_; }
    const std::string& lastTransport() const { return lastTransport_; }

private:
    class Fanout;

    std::unique_ptr<IByteStream> connectWithRetry(const ConnectionInfo& info);
    bool sleepCancellable(int seconds);
    void report(bool success, const std::string& message);

    std::vector<IProgressObserver*> observers_;
    std::unique_ptr<Fanout> fanout_;
    StreamFactory streamFactory_;
    std::string senderName_;
    int connectTimeoutSeconds_ = MAX_CONNECT_TIMEOUT_SECONDS;
    int retryDelaySeconds_ = RETRY_DELAY_SECONDS;

    CancelToken cancel_;
    std::mutex streamMutex_;
    IByteStream* activeStream_ = nullptr;

    bool lastSucceeded_ = false;
    std::string lastMessage_;
    std::string lastTransport_;
};
