#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Observer for transfer progress. Only onProgress() is mandatory, so existing
// observers keep compiling; the richer hooks default to no-ops.
// All callbacks run on the thread that performs the transfer.
class IProgressObserver {
public:
    // Overall session progress, 0-100 (deduplicated by the engine).
    virtual void onProgress(int percent) = 0;
    virtual void onError(const std::string& message) { (void)message; }

    virtual void onSessionStarted(std::size_t fileCount, uint64_t totalBytes) {
        (void)fileCount; (void)totalBytes;
    }
    virtual void onFileStarted(std::size_t index, const std::string& name, uint64_t size) {
        (void)index; (void)name; (void)size;
    }
    // Called after every chunk; implementations should throttle UI work themselves.
    virtual void onBytesTransferred(uint64_t sessionBytes, uint64_t sessionTotal) {
        (void)sessionBytes; (void)sessionTotal;
    }
    // Sender: called only after the receiver acknowledged the checksum.
    // Receiver: savedPath is the final (non-overwriting) path on disk.
    virtual void onFileCompleted(std::size_t index, const std::string& name,
                                 const std::string& savedPath, bool verified) {
        (void)index; (void)name; (void)savedPath; (void)verified;
    }
    virtual void onSessionFinished(bool success, const std::string& message) {
        (void)success; (void)message;
    }

    virtual ~IProgressObserver() = default;
};
