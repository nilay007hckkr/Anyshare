#include <iostream>
#include <memory>
#include "NetworkEngine.hpp"
#include "LargeFileTestHandover.hpp"

class ConsoleObserver : public IProgressObserver {
public:
    void onProgress(int percent) override {
        std::cout << "Transfer progress: " << percent << "%" << std::endl;
    }

    void onError(const std::string& message) override {
        std::cerr << "ERROR: " << message << std::endl;
    }
};

int main() {
    ConsoleObserver consoleObserver;
    NetworkEngine engine;
    engine.addObserver(&consoleObserver);
    engine.startTransfer(std::make_unique<LargeFileTestHandover>());
    return 0;
}
