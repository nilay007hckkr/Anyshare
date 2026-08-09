#pragma once
// ============================================================================
// IProgressObserver.hpp — Observer Pattern (GoF)
// ============================================================================
// OOP Principle: ABSTRACTION + POLYMORPHISM
// This pure virtual interface defines a contract for progress notification.
// Any concrete observer (console UI, Android JNI callback, GUI progress bar)
// can implement this interface without the NetworkEngine knowing or caring
// about the concrete type — runtime polymorphism via vtable dispatch.
// ============================================================================

class IProgressObserver {
public:
    // Called by the subject (NetworkEngine) whenever transfer progress changes.
    // percent: 0–100 for normal progress, -1 signals a fatal error.
    virtual void onProgress(int percent) = 0;

    // OOP Principle: Virtual destructor ensures correct cleanup when deleting
    // through a base-class pointer (prevents resource leaks in derived classes).
    virtual ~IProgressObserver() = default;
};
