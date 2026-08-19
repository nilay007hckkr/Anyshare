#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <array>
#include <fstream>
#include <chrono>
#include <string>
#include "IProgressObserver.hpp"
#include "INfcHandover.hpp"
#include "Checksum.hpp"

class NetworkEngine {
private:
    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer retryTimer_;
    std::chrono::steady_clock::time_point connectStartTime_;

    std::vector<IProgressObserver*> observers_;
    ConnectionInfo connectionInfo_;

    std::array<char, 65536> buffer_;
    std::ifstream sourceFile_;
    uint64_t bytesTransferredThisFile_ = 0;
    size_t currentFileIndex_ = 0;
    int lastReportedPercent_ = -1;
    Crc32Calculator currentFileChecksum_;

    int fileRetryCount_ = 0;
    bool isResumingFile_ = false;

    static constexpr int RETRY_DELAY_SECONDS = 2;
    static constexpr int MAX_CONNECT_TIMEOUT_SECONDS = 60;
    static constexpr int MAX_FILE_RETRIES = 3;

    void notifyObservers(int percent);
    void notifyObserversError(const std::string& message);
    void attemptConnect();
    void handleConnectResult(const boost::system::error_code& error);
    void startNextFile();
    void openAndBeginCurrentFile();
    void sendNextChunk();
    void handleWriteResult(const boost::system::error_code& error, std::size_t bytesWritten);
    void handleTransferError(const std::string& reason);
    void finishCurrentFile();
    void abortTransfer(const std::string& reason);

public:
    NetworkEngine();
    void addObserver(IProgressObserver* observer);
    void connectToHost(const ConnectionInfo& info);
    void startTransfer(std::unique_ptr<INfcHandover> handoverStrategy);
};
