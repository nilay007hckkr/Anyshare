// Minimal ASP/1 receiver for the demo / stress_test executables (replaces the
// old `nc -l 8080 > received.txt`, which could not verify anything).
//
//   ./receiver [port=8080] [dir=received] [bind=127.0.0.1]
//
// Test tool: it accepts every offer. The full Linux app (linux/anyshare)
// asks before accepting unless the sender proves it read the tap payload.
#include <iostream>
#include <string>

#include "SocketStream.hpp"
#include "TransferSession.hpp"

class ConsoleObserver : public IProgressObserver {
public:
    void onProgress(int percent) override {
        if (percent % 10 == 0) std::cout << "Receive progress: " << percent << "%" << std::endl;
    }
    void onError(const std::string& message) override { std::cerr << "ERROR: " << message << std::endl; }
    void onFileCompleted(std::size_t, const std::string& name, const std::string& path, bool verified) override {
        std::cout << (verified ? "Verified OK: " : "Unverified: ") << name << " -> " << path << std::endl;
    }
};

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
    std::string dir = argc > 2 ? argv[2] : "received";
    std::string bind = argc > 3 ? argv[3] : "127.0.0.1";

    anyshare::TcpListener listener(bind, port);
    std::cout << "Listening on " << bind << ":" << listener.port() << ", saving to " << dir << std::endl;
    auto stream = listener.accept(-1);
    if (!stream) return 1;

    anyshare::ReceiveOptions options;
    options.destinationDir = dir;
    options.acceptOffer = [](const anyshare::protocol::Offer& offer) {
        std::cout << "Accepting " << offer.files.size() << " file(s) from '" << offer.senderName << "'" << std::endl;
        return true;
    };
    ConsoleObserver observer;
    auto result = anyshare::receiveFiles(*stream, options, &observer);
    std::cout << anyshare::toString(result.status) << ": " << result.message << std::endl;
    return result.status == anyshare::TransferStatus::Success ? 0 : 1;
}
