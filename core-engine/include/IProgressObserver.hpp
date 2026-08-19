#pragma once

#include <string>

class IProgressObserver {
public:
    virtual void onProgress(int percent) = 0;
    virtual void onError(const std::string& message) {}
    virtual ~IProgressObserver() = default;
};
