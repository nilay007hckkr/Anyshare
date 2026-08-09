// ============================================================================
// NetworkEngine.cpp — Core async file transfer engine (implementation)
// ============================================================================
// This file implements the full async chain:
//   startTransfer() → doConnect() → onConnect() → doReadChunk() →
//   doWriteChunk() → doReadChunk() → ... → onTransferComplete()
//
// OOP principles and design patterns are called out inline.
// ============================================================================

#include "NetworkEngine.hpp"

#include <functional>   // std::bind, std::placeholders
#include <chrono>

// ============================================================================
// Constructor / Destructor
// ============================================================================

// OOP Principle: ENCAPSULATION — all state is initialized in the member
// initializer list. The io_context owns the socket and timer lifetimes.
NetworkEngine::NetworkEngine(int connect_timeout_seconds)
    : io_context_()
    , socket_(io_context_)                                  // RAII: socket bound to io_context
    , retry_timer_(io_context_)                             // RAII: timer bound to io_context
    , bytes_sent_(0)
    , max_retries_(connect_timeout_seconds / 2)             // 2s per retry
    , retry_count_(0)
    , last_reported_percent_(-1)
{
}

// RAII: destructor ensures clean teardown of networking resources.
NetworkEngine::~NetworkEngine() {
    shutdown();
}

// ============================================================================
// Observer Pattern — registration and notification
// ============================================================================

// OOP Principle: POLYMORPHISM — observers are stored as base-class pointers.
// The engine calls onProgress() through the interface; the correct derived
// implementation is dispatched at runtime via the vtable.
void NetworkEngine::addObserver(IProgressObserver* observer) {
    if (observer) {
        observers_.push_back(observer);
    }
}

// Notify all registered observers of the current transfer progress.
// Deduplicates: only fires when percent actually changes.
void NetworkEngine::notifyObservers(int percent) {
    if (percent == last_reported_percent_) {
        return;  // avoid spamming observers with duplicate updates
    }
    last_reported_percent_ = percent;

    // OOP Principle: POLYMORPHISM — each observer->onProgress() call
    // dispatches to the correct concrete implementation at runtime.
    for (IProgressObserver* obs : observers_) {
        obs->onProgress(percent);
    }
}

// ============================================================================
// Strategy Pattern — startTransfer()
// ============================================================================

// OOP Principle: POLYMORPHISM + ABSTRACTION (Strategy Pattern)
// The engine accepts ANY concrete INfcHandover via std::unique_ptr (ownership
// transfer). It calls performHandover() through the abstract interface —
// the concrete strategy (JsonHandover, LinuxNfcHandover, AndroidNfcHandover)
// determines HOW connection info is obtained.
void NetworkEngine::startTransfer(std::unique_ptr<INfcHandover> handover_strategy) {
    // --- Strategy Pattern: obtain connection info via pluggable strategy ---
    connection_info_ = handover_strategy->performHandover();

    // The strategy's job is done; unique_ptr releases it automatically (RAII).

    std::cout << "[Engine] Connection info received via NFC handover strategy:\n"
              << "         SSID:     " << connection_info_.ssid << "\n"
              << "         IP:       " << connection_info_.ip << "\n"
              << "         Port:     " << connection_info_.port << "\n"
              << "         Filename: " << connection_info_.filename << "\n"
              << "         Filesize: " << connection_info_.filesize << " bytes\n";

    // Open the source file in binary mode (RAII — ifstream closes on destruction).
    file_.open(connection_info_.filename, std::ios::binary);
    if (!file_.is_open()) {
        std::cerr << "[Engine] ERROR: Cannot open file: "
                  << connection_info_.filename << "\n";
        notifyObservers(-1);  // -1 signals fatal error to observers
        return;
    }

    // Resolve the remote endpoint from ConnectionInfo.
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(connection_info_.ip, ec);
    if (ec) {
        std::cerr << "[Engine] ERROR: Invalid IP address: "
                  << connection_info_.ip << " — " << ec.message() << "\n";
        notifyObservers(-1);
        return;
    }
    remote_endpoint_ = boost::asio::ip::tcp::endpoint(address, connection_info_.port);

    // Reset transfer state.
    bytes_sent_ = 0;
    retry_count_ = 0;
    last_reported_percent_ = -1;

    // Notify observers that transfer is starting.
    notifyObservers(0);

    // Kick off the async connect chain.
    doConnect();
}

// ============================================================================
// Async Event Loop
// ============================================================================

// Drives all async operations. Blocks until the transfer completes or fails.
void NetworkEngine::run() {
    io_context_.run();
}

// ============================================================================
// Step 1: Async Connect with Retry
// ============================================================================

// Initiates a non-blocking TCP connect to the Wi-Fi Direct Group Owner.
// On failure, retries every 2 seconds up to max_retries_ (configurable).
// Uses boost::asio::steady_timer for non-blocking waits (no sleep()!).
void NetworkEngine::doConnect() {
    std::cout << "[Engine] Connecting to " << remote_endpoint_
              << " (attempt " << (retry_count_ + 1)
              << "/" << max_retries_ << ")...\n";

    // async_connect posts the connect operation to the io_context.
    // The lambda callback fires when the OS reports success or failure.
    socket_.async_connect(
        remote_endpoint_,
        [this](const boost::system::error_code& ec) {
            onConnect(ec);
        }
    );
}

// ============================================================================
// Step 2: Handle Connect Result
// ============================================================================

void NetworkEngine::onConnect(const boost::system::error_code& ec) {
    if (!ec) {
        // --- SUCCESS: connected to peer ---
        std::cout << "[Engine] Connected to " << remote_endpoint_ << "!\n";
        // Start the streaming chain.
        doReadChunk();
        return;
    }

    // --- FAILURE: connection refused / timeout / unreachable ---
    std::cerr << "[Engine] Connect failed: " << ec.message() << "\n";

    ++retry_count_;
    if (retry_count_ >= max_retries_) {
        // All retries exhausted — give up gracefully.
        std::cerr << "[Engine] Max retries (" << max_retries_
                  << ") exhausted. Giving up.\n";
        notifyObservers(-1);
        shutdown();
        return;
    }

    // Close the failed socket so we can reuse it on the next attempt.
    boost::system::error_code close_ec;
    socket_.close(close_ec);

    // Non-blocking 2-second wait using steady_timer (NOT std::this_thread::sleep_for).
    std::cout << "[Engine] Retrying in 2 seconds...\n";
    retry_timer_.expires_after(std::chrono::seconds(2));
    retry_timer_.async_wait(
        [this](const boost::system::error_code& timer_ec) {
            if (!timer_ec) {
                doConnect();  // retry
            } else {
                std::cerr << "[Engine] Retry timer cancelled: "
                          << timer_ec.message() << "\n";
            }
        }
    );
}

// ============================================================================
// Step 3: Read a Chunk from the File
// ============================================================================

// Reads up to 64KB from the source file into buffer_.
// File I/O is synchronous (ifstream::read) but bounded to kBufferSize bytes,
// so memory usage stays flat regardless of file size (OOM-avoidance).
void NetworkEngine::doReadChunk() {
    // Read the next chunk from the file.
    file_.read(buffer_.data(), static_cast<std::streamsize>(kBufferSize));
    std::size_t bytes_read = static_cast<std::size_t>(file_.gcount());

    if (bytes_read > 0) {
        // More data to send — chain into async_write.
        doWriteChunk(bytes_read);
    } else {
        // End of file — transfer is complete.
        onTransferComplete();
    }
}

// ============================================================================
// Step 4: Write a Chunk to the Socket (Async)
// ============================================================================

// Uses boost::asio::async_write (NOT async_write_some) to guarantee the
// ENTIRE chunk is written before proceeding. This avoids partial-write bugs.
void NetworkEngine::doWriteChunk(std::size_t bytes_read) {
    // async_write chains internally until all bytes_read bytes are flushed.
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(buffer_.data(), bytes_read),
        [this](const boost::system::error_code& ec, std::size_t bytes_written) {
            if (ec) {
                std::cerr << "[Engine] Write error: " << ec.message() << "\n";
                notifyObservers(-1);
                shutdown();
                return;
            }

            // Update running total and notify observers.
            bytes_sent_ += bytes_written;

            // Progress = (bytes_sent * 100) / total_filesize
            // Guard against division by zero (shouldn't happen, but defensive).
            int percent = 0;
            if (connection_info_.filesize > 0) {
                percent = static_cast<int>(
                    (bytes_sent_ * 100) / connection_info_.filesize
                );
            }

            // Observer Pattern: broadcast real progress to all observers.
            notifyObservers(percent);

            // Chain: read next chunk from file.
            doReadChunk();
        }
    );
}

// ============================================================================
// Step 5: Transfer Complete
// ============================================================================

void NetworkEngine::onTransferComplete() {
    std::cout << "[Engine] Transfer complete! " << bytes_sent_
              << " bytes sent.\n";

    // Ensure we report exactly 100% on completion.
    notifyObservers(100);

    // RAII cleanup.
    shutdown();
}

// ============================================================================
// RAII Cleanup
// ============================================================================

// OOP Principle: ENCAPSULATION — cleanup logic is private and automatic.
// Called from destructor, onTransferComplete(), and error paths.
void NetworkEngine::shutdown() {
    // Close the file stream if open.
    if (file_.is_open()) {
        file_.close();
    }

    // Gracefully shut down and close the TCP socket.
    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        // Ignore ec — shutdown on a non-connected socket is expected to fail.
        socket_.close(ec);
    }

    // Cancel any pending timer.
    retry_timer_.cancel();
}
