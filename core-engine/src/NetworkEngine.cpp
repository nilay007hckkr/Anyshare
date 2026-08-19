#include "NetworkEngine.hpp"
#include <iostream>

NetworkEngine::NetworkEngine()
    : socket_(io_context_), retryTimer_(io_context_) {}

void NetworkEngine::addObserver(IProgressObserver* observer) {
    observers_.push_back(observer);
}

void NetworkEngine::notifyObservers(int percent) {
    for (IProgressObserver* observer : observers_) {
        observer->onProgress(percent);
    }
}

void NetworkEngine::notifyObserversError(const std::string& message) {
    for (IProgressObserver* observer : observers_) {
        observer->onError(message);
    }
}

void NetworkEngine::attemptConnect() {
    try {
        boost::asio::ip::tcp::endpoint endpoint(
            boost::asio::ip::make_address(connectionInfo_.ip),
            connectionInfo_.port
        );

        socket_.async_connect(endpoint,
            [this](const boost::system::error_code& error) {
                handleConnectResult(error);
            });
    } catch (const std::exception& e) {
        abortTransfer(std::string("Invalid connection info: ") + e.what());
    }
}

void NetworkEngine::handleConnectResult(const boost::system::error_code& error) {
    if (!error) {
        std::cout << "Connected successfully to "
                  << connectionInfo_.ip << ":" << connectionInfo_.port << std::endl;

        if (isResumingFile_) {
            isResumingFile_ = false;
            openAndBeginCurrentFile();
        } else {
            startNextFile();
        }
        return;
    }

    auto elapsed = std::chrono::steady_clock::now() - connectStartTime_;
    if (elapsed > std::chrono::seconds(MAX_CONNECT_TIMEOUT_SECONDS)) {
        abortTransfer("Connection failed permanently after " +
                       std::to_string(MAX_CONNECT_TIMEOUT_SECONDS) + " seconds");
        return;
    }

    std::cerr << "Connect attempt failed (" << error.message()
              << "). Retrying in " << RETRY_DELAY_SECONDS << "s..." << std::endl;

    retryTimer_.expires_after(std::chrono::seconds(RETRY_DELAY_SECONDS));
    retryTimer_.async_wait([this](const boost::system::error_code& timerError) {
        if (!timerError) {
            socket_.close();
            attemptConnect();
        }
    });
}

void NetworkEngine::startNextFile() {
    if (currentFileIndex_ >= connectionInfo_.files.size()) {
        std::cout << "All files sent. Session complete." << std::endl;
        return;
    }

    fileRetryCount_ = 0;
    openAndBeginCurrentFile();
}

void NetworkEngine::openAndBeginCurrentFile() {
    const FileMetadata& file = connectionInfo_.files[currentFileIndex_];

    sourceFile_.open(file.filename, std::ios::binary);
    if (!sourceFile_.is_open()) {
        notifyObserversError("Failed to open file: " + file.filename);
        currentFileIndex_++;
        startNextFile();
        return;
    }

    bytesTransferredThisFile_ = 0;
    lastReportedPercent_ = -1;
    currentFileChecksum_ = Crc32Calculator();

    std::cout << "Sending file " << (currentFileIndex_ + 1) << "/"
              << connectionInfo_.files.size() << ": " << file.filename << std::endl;

    sendNextChunk();
}

void NetworkEngine::sendNextChunk() {
    sourceFile_.read(buffer_.data(), buffer_.size());
    std::streamsize bytesRead = sourceFile_.gcount();

    if (bytesRead <= 0) {
        finishCurrentFile();
        return;
    }

    currentFileChecksum_.update(buffer_.data(), static_cast<size_t>(bytesRead));

    boost::asio::async_write(socket_, boost::asio::buffer(buffer_.data(), bytesRead),
        [this](const boost::system::error_code& error, std::size_t bytesWritten) {
            handleWriteResult(error, bytesWritten);
        });
}

void NetworkEngine::handleWriteResult(const boost::system::error_code& error, std::size_t bytesWritten) {
    if (error) {
        handleTransferError("Write failed: " + error.message());
        return;
    }

    bytesTransferredThisFile_ += bytesWritten;

    const FileMetadata& file = connectionInfo_.files[currentFileIndex_];
    int percent = static_cast<int>((bytesTransferredThisFile_ * 100) / file.filesize);

    if (percent != lastReportedPercent_) {
        lastReportedPercent_ = percent;
        notifyObservers(percent);
    }

    sendNextChunk();
}

void NetworkEngine::handleTransferError(const std::string& reason) {
    fileRetryCount_++;

    if (fileRetryCount_ > MAX_FILE_RETRIES) {
        abortTransfer(reason + " -- giving up after " + std::to_string(MAX_FILE_RETRIES) + " retries");
        return;
    }

    notifyObserversError(reason + ". Retrying file from the start (attempt " +
                          std::to_string(fileRetryCount_) + "/" +
                          std::to_string(MAX_FILE_RETRIES) + ") in " +
                          std::to_string(RETRY_DELAY_SECONDS) + "s...");

    if (sourceFile_.is_open()) {
        sourceFile_.close();
    }
    boost::system::error_code ignored;
    socket_.close(ignored);

    isResumingFile_ = true;

    retryTimer_.expires_after(std::chrono::seconds(RETRY_DELAY_SECONDS));
    retryTimer_.async_wait([this](const boost::system::error_code& timerError) {
        if (!timerError) {
            connectStartTime_ = std::chrono::steady_clock::now();
            attemptConnect();
        }
    });
}

void NetworkEngine::finishCurrentFile() {
    sourceFile_.close();

    const FileMetadata& file = connectionInfo_.files[currentFileIndex_];
    std::string computedChecksum = currentFileChecksum_.getHexValue();

    if (computedChecksum == file.checksum) {
        std::cout << "Verified OK: " << file.filename
                  << " (checksum " << computedChecksum << ")" << std::endl;
    } else {
        notifyObserversError("Checksum mismatch for " + file.filename +
                              " -- expected " + file.checksum +
                              ", got " + computedChecksum);
    }

    currentFileIndex_++;
    startNextFile();
}

void NetworkEngine::abortTransfer(const std::string& reason) {
    boost::system::error_code ignored;
    socket_.close(ignored);

    if (sourceFile_.is_open()) {
        sourceFile_.close();
    }

    notifyObserversError(reason);
}

void NetworkEngine::connectToHost(const ConnectionInfo& info) {
    connectionInfo_ = info;
    currentFileIndex_ = 0;
    connectStartTime_ = std::chrono::steady_clock::now();
    attemptConnect();
    io_context_.run();
}

void NetworkEngine::startTransfer(std::unique_ptr<INfcHandover> handoverStrategy) {
    std::cout << "Requesting connection info via handover strategy..." << std::endl;
    try {
        ConnectionInfo info = handoverStrategy->performHandover();
        connectToHost(info);
    } catch (const std::exception& e) {
        notifyObserversError(std::string("Handover failed: ") + e.what());
    }
}
