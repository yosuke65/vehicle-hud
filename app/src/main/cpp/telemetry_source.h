#pragma once

#include <atomic>
#include <mutex>
#include <thread>

// Latest-value telemetry sample. The render thread only ever wants the
// freshest reading, never a queued backlog, so this is intentionally a
// single shared slot rather than a producer/consumer queue.
struct TelemetrySample {
    float speedKph = 0.0f;
    float rpm = 0.0f;
};

// Generates fake vehicle telemetry on its own thread at ~10Hz and exposes
// the latest sample to any reader. A short-held mutex protects the shared
// slot: a writer locks only to assign two floats, and latest() locks only
// to copy them out before returning — no lock is ever held across a GL
// call on the render thread's side, and no reader blocks waiting for a
// new sample (it always gets whatever is freshest, which is what a HUD
// needle actually wants).
//
// setExternal() lets a second, independent source (the AIDL
// TelemetryService, via JNI — see native_bridge.cpp) drive this same slot.
// The internal generator checks useExternal_ each tick and stops writing
// once external data has arrived, so the two sources hand off cleanly
// instead of fighting over the same value every tick.
class TelemetrySource {
public:
    TelemetrySource();
    ~TelemetrySource();

    TelemetrySource(const TelemetrySource&) = delete;
    TelemetrySource& operator=(const TelemetrySource&) = delete;

    void start();
    // Signals the loop to exit and blocks until the thread returns. Safe
    // to call more than once.
    void stop();

    TelemetrySample latest() const;

    // Called from the AIDL-polling thread via JNI. After the first call,
    // the internal fake generator stops overwriting this slot.
    void setExternal(float speedKph, float rpm);

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> useExternal_{false};

    mutable std::mutex mutex_;
    TelemetrySample sample_;
};
