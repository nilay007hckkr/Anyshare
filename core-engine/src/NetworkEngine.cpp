#include "NetworkEngine.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include "SocketStream.hpp"

// Re-bases progress from each (re)connection attempt onto the whole job so
// observers see one monotonic 0-100% session even across reconnects.
class NetworkEngine::Fanout : public IProgressObserver {
public:
    explicit Fanout(std::vector<IProgressObserver*>& observers) : observers_(observers) {}

    void beginJob(std::size_t fileCount, uint64_t totalBytes) {
        fileCount_ = fileCount;
        totalBytes_ = totalBytes;
        baseIndex_ = 0;
        baseBytes_ = 0;
        lastPercent_ = -1;
        started_ = false;
    }
    void setBase(std::size_t index, uint64_t bytes) {
        baseIndex_ = index;
        baseBytes_ = bytes;
    }

    void onProgress(int) override {}   // recomputed from bytes below
    void onError(const std::string& message) override {
        for (auto* o : observers_) o->onError(message);
    }
    void onSessionStarted(std::size_t, uint64_t) override {
        if (started_) return;
        started_ = true;
        for (auto* o : observers_) o->onSessionStarted(fileCount_, totalBytes_);
    }
    void onFileStarted(std::size_t index, const std::string& name, uint64_t size) override {
        for (auto* o : observers_) o->onFileStarted(baseIndex_ + index, name, size);
    }
    void onBytesTransferred(uint64_t sessionBytes, uint64_t) override {
        uint64_t done = baseBytes_ + sessionBytes;
        int percent = totalBytes_ == 0 ? 100 : static_cast<int>((done * 100) / totalBytes_);
        for (auto* o : observers_) o->onBytesTransferred(done, totalBytes_);
        if (percent != lastPercent_) {
            lastPercent_ = percent;
            for (auto* o : observers_) o->onProgress(percent);
        }
    }
    void onFileCompleted(std::size_t index, const std::string& name, const std::string& path, bool verified) override {
        for (auto* o : observers_) o->onFileCompleted(baseIndex_ + index, name, path, verified);
    }
    void onSessionFinished(bool, const std::string&) override {}   // the engine reports the final outcome

private:
    std::vector<IProgressObserver*>& observers_;
    std::size_t fileCount_ = 0;
    uint64_t totalBytes_ = 0;
    std::size_t baseIndex_ = 0;
    uint64_t baseBytes_ = 0;
    int lastPercent_ = -1;
    bool started_ = false;
};

NetworkEngine::NetworkEngine() : fanout_(std::make_unique<Fanout>(observers_)) {
    streamFactory_ = [](const ConnectionInfo& info, int timeoutMs) -> std::unique_ptr<IByteStream> {
        if (info.transport == "tcp") {
            if (info.ip.empty() || info.port == 0) throw TransportError("no TCP address in connection info");
            return anyshare::connectTcp(info.ip, info.port, timeoutMs);
        }
        throw TransportError("transport '" + info.transport + "' is not available in this build");
    };
}

NetworkEngine::~NetworkEngine() = default;

void NetworkEngine::addObserver(IProgressObserver* observer) {
    if (observer) observers_.push_back(observer);
}

void NetworkEngine::cancel() {
    cancel_.cancel();
    std::lock_guard<std::mutex> lock(streamMutex_);
    if (activeStream_) activeStream_->abort();
}

bool NetworkEngine::sleepCancellable(int seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_.isCancelled()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !cancel_.isCancelled();
}

void NetworkEngine::report(bool success, const std::string& message) {
    lastSucceeded_ = success;
    lastMessage_ = message;
    for (auto* o : observers_) o->onSessionFinished(success, message);
}

std::unique_ptr<IByteStream> NetworkEngine::connectWithRetry(const ConnectionInfo& info) {
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(connectTimeoutSeconds_);
    for (int attempt = 1;; ++attempt) {
        if (cancel_.isCancelled()) return nullptr;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        int timeoutMs = static_cast<int>(std::max<long long>(1000, std::min<long long>(left.count(), 10000)));
        try {
            auto stream = streamFactory_(info, timeoutMs);
            std::cout << "Connected via " << stream->describe() << std::endl;
            return stream;
        } catch (const TransportError& e) {
            if (cancel_.isCancelled()) return nullptr;
            if (std::chrono::steady_clock::now() + std::chrono::seconds(retryDelaySeconds_) > deadline) {
                fanout_->onError("Connection failed permanently after " + std::to_string(connectTimeoutSeconds_) +
                                 " seconds: " + e.what());
                return nullptr;
            }
            std::cerr << "Connect attempt " << attempt << " failed (" << e.what() << "). Retrying in "
                      << retryDelaySeconds_ << "s..." << std::endl;
            if (!sleepCancellable(retryDelaySeconds_)) return nullptr;
        }
    }
}

void NetworkEngine::connectToHost(const ConnectionInfo& info) {
    cancel_.reset();
    lastSucceeded_ = false;
    lastTransport_.clear();

    // Validate every file before touching the network, so a typo never
    // results in a partially delivered set.
    std::vector<anyshare::OutgoingFile> files;
    uint64_t totalBytes = 0;
    for (const auto& meta : info.files) {
        try {
            files.push_back(anyshare::outgoingFileFromPath(meta.filename, meta.mimeType, meta.checksum));
            totalBytes += files.back().size;
        } catch (const std::exception& e) {
            std::string message = std::string("Failed to open file: ") + e.what();
            fanout_->onError(message);
            report(false, message);
            return;
        }
    }
    if (files.empty()) {
        report(false, "No files to send");
        return;
    }
    fanout_->beginJob(files.size(), totalBytes);

    std::size_t done = 0;
    uint64_t doneBytes = 0;
    for (int retry = 0;; ++retry) {
        auto stream = connectWithRetry(info);
        if (!stream) {
            report(false, cancel_.isCancelled() ? "Cancelled" : "Could not connect to receiver");
            return;
        }
        lastTransport_ = stream->describe();
        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            activeStream_ = stream.get();
        }

        std::vector<anyshare::OutgoingFile> remaining(files.begin() + static_cast<std::ptrdiff_t>(done), files.end());
        fanout_->setBase(done, doneBytes);
        anyshare::SendOptions options;
        options.senderName = senderName_;
        options.token = info.sessionToken;
        anyshare::SendResult result = anyshare::sendFiles(*stream, remaining, options, fanout_.get(), &cancel_);

        {
            std::lock_guard<std::mutex> lock(streamMutex_);
            activeStream_ = nullptr;
        }
        for (std::size_t i = 0; i < result.filesCompleted; ++i) doneBytes += files[done + i].size;
        done += result.filesCompleted;

        if (result.status == anyshare::TransferStatus::Success) {
            report(true, "All " + std::to_string(files.size()) + " file(s) delivered and verified via " + lastTransport_);
            return;
        }
        if (!result.retryable() || cancel_.isCancelled()) {
            report(false, result.message);
            return;
        }
        if (retry + 1 > MAX_FILE_RETRIES) {
            report(false, result.message + " -- giving up after " + std::to_string(MAX_FILE_RETRIES) + " retries");
            return;
        }
        fanout_->onError("Reconnecting to resend " + std::to_string(files.size() - done) + " unconfirmed file(s) (attempt " +
                         std::to_string(retry + 1) + "/" + std::to_string(MAX_FILE_RETRIES) + ")");
        if (!sleepCancellable(retryDelaySeconds_)) {
            report(false, "Cancelled");
            return;
        }
    }
}

void NetworkEngine::startTransfer(std::unique_ptr<INfcHandover> handoverStrategy) {
    std::cout << "Requesting connection info via " << handoverStrategy->describe() << "..." << std::endl;
    ConnectionInfo info;
    try {
        info = handoverStrategy->performHandover();
    } catch (const std::exception& e) {
        std::string message = std::string("Handover failed: ") + e.what();
        fanout_->onError(message);
        report(false, message);
        return;
    }
    connectToHost(info);
}
