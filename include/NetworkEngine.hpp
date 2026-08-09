#pragma once
// ============================================================================
// NetworkEngine.hpp — Core async file transfer engine
// ============================================================================
// OOP Principle: ENCAPSULATION
// All networking state (socket, timer, buffer, file handle, transfer counters)
// is private. The public API exposes only what callers need: addObserver(),
// startTransfer(), and run(). Internal async machinery is completely hidden.
//
// OOP Principle: COMPOSITION ("has-a" over "is-a")
// NetworkEngine *has* a socket, a timer, a buffer, a list of observers, and
// a connection info struct. It does NOT inherit from any networking base class.
// This keeps the class focused and testable.
// ============================================================================

#include <boost/asio.hpp>
#include <array>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include "IProgressObserver.hpp"
#include "INfcHandover.hpp"

class NetworkEngine {
public:
    // -----------------------------------------------------------------------
    // Constructor / Destructor
    // -----------------------------------------------------------------------

    // OOP Principle: ENCAPSULATION — initializes all RAII members via the
    // member initializer list. The io_context owns the socket and timer
    // lifetimes, ensuring deterministic cleanup.
    explicit NetworkEngine(int connect_timeout_seconds = 30);

    // RAII: destructor calls shutdown() to close socket + file gracefully.
    ~NetworkEngine();

    // Non-copyable, non-movable (owns async state tied to io_context).
    NetworkEngine(const NetworkEngine&) = delete;
    NetworkEngine& operator=(const NetworkEngine&) = delete;

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    // Observer Pattern: register a concrete observer to receive progress
    // updates. Multiple observers can be attached (e.g., console + GUI).
    // Raw pointer is intentional — engine does NOT own observers.
    void addObserver(IProgressObserver* observer);

    // Strategy Pattern entry point.
    // Accepts any concrete INfcHandover implementation via std::unique_ptr
    // (OOP: POLYMORPHISM — the engine calls performHandover() through the
    // interface without knowing the concrete type).
    // After obtaining ConnectionInfo, opens the source file and kicks off
    // the async connect → stream chain.
    void startTransfer(std::unique_ptr<INfcHandover> handover_strategy);

    // Drives the Boost.Asio event loop. Blocks until the transfer completes
    // (or fails). All async callbacks execute within this call.
    void run();

private:
    // -----------------------------------------------------------------------
    // Async chain methods (private implementation detail)
    // -----------------------------------------------------------------------

    // Step 1: Initiate async_connect to target IP:port.
    void doConnect();

    // Step 2: Handle connect result — retry on failure, start streaming on success.
    void onConnect(const boost::system::error_code& ec);

    // Step 3: Read the next 64KB chunk from the file (synchronous file I/O).
    void doReadChunk();

    // Step 4: Write the chunk to the socket asynchronously, then loop back to Step 3.
    void doWriteChunk(std::size_t bytes_read);

    // Step 5: Final cleanup on success.
    void onTransferComplete();

    // Gracefully close socket + file.
    void shutdown();

    // Observer Pattern: notify all registered observers of current progress.
    void notifyObservers(int percent);

    // -----------------------------------------------------------------------
    // Private data members — ENCAPSULATION
    // -----------------------------------------------------------------------

    // Boost.Asio I/O context — the event loop that drives all async operations.
    boost::asio::io_context io_context_;

    // TCP socket — RAII managed, auto-closed when engine is destroyed.
    boost::asio::ip::tcp::socket socket_;

    // Steady timer for non-blocking retry delays (no sleep()!).
    boost::asio::steady_timer retry_timer_;

    // OOP Principle: COMPOSITION — engine *has* a list of observers.
    // Raw pointers because the engine does not own observer lifetimes.
    std::vector<IProgressObserver*> observers_;

    // Connection details populated by the INfcHandover strategy.
    ConnectionInfo connection_info_;

    // -----------------------------------------------------------------------
    // Streaming state
    // -----------------------------------------------------------------------

    // Fixed 64KB buffer — prevents OOM by never loading the entire file.
    // std::array is stack-allocated, deterministic, no heap fragmentation.
    static constexpr std::size_t kBufferSize = 65536;
    std::array<char, kBufferSize> buffer_;

    // Source file stream (binary mode). RAII — auto-closed on destruction.
    std::ifstream file_;

    // Running total of bytes sent so far (for progress calculation).
    uint64_t bytes_sent_;

    // -----------------------------------------------------------------------
    // Retry state
    // -----------------------------------------------------------------------

    // Maximum number of connect retries (timeout_seconds / 2).
    int max_retries_;

    // Current retry attempt (0-based).
    int retry_count_;

    // Resolved remote endpoint (computed once from ConnectionInfo).
    boost::asio::ip::tcp::endpoint remote_endpoint_;

    // Track the last reported progress percent to avoid spamming observers
    // with duplicate notifications (important for large files with many chunks).
    int last_reported_percent_;
};
