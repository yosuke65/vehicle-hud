#include "telemetry_source.h"

#include <chrono>
#include <cmath>

// This file is compiled two ways: normally via the app's NDK/CMake build
// (__ANDROID__ defined by the toolchain), and standalone by
// native_tests/CMakeLists.txt on the host, with GoogleTest and (optionally)
// ThreadSanitizer, to test the mutex-guarded handoff logic directly against
// this exact production source rather than a reimplementation. Only the
// logging call differs between the two.
#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "VehicleHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

namespace {
constexpr float kMaxSpeedKph = 180.0f;
constexpr float kMaxRpm = 6000.0f;
constexpr auto kTickInterval = std::chrono::milliseconds(100); // ~10Hz
} // namespace

TelemetrySource::TelemetrySource() = default;

TelemetrySource::~TelemetrySource() {
    stop();
}

void TelemetrySource::start() {
    if (running_.exchange(true)) {
        return; // already running
    }
    thread_ = std::thread(&TelemetrySource::run, this);
}

void TelemetrySource::stop() {
    if (!running_.exchange(false)) {
        return; // already stopped, or never started
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

TelemetrySample TelemetrySource::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sample_; // copy out, lock released on return
}

void TelemetrySource::setExternal(float speedKph, float rpm) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_.speedKph = speedKph;
        sample_.rpm = rpm;
    }
    const bool wasAlreadyExternal = useExternal_.exchange(true);
    if (!wasAlreadyExternal) {
        LOGI("External telemetry now driving this source"); // log the handoff once, not every poll tick
    }
}

void TelemetrySource::run() {
    LOGI("Telemetry thread started");
    const auto start = std::chrono::steady_clock::now();

    while (running_.load()) {
        if (!useExternal_.load()) {
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - start).count();

            // Fake driving cycle: speed and RPM each oscillate smoothly
            // but out of phase, so the two don't move in perfect lockstep
            // — closer to how the real gauges would actually behave than
            // a single shared sine value would look. Stops once
            // setExternal() has been called at least once.
            const float speed = (std::sin(elapsed * 0.4f) * 0.5f + 0.5f) * kMaxSpeedKph;
            const float rpm = (std::sin(elapsed * 0.9f + 1.0f) * 0.5f + 0.5f) * kMaxRpm;

            std::lock_guard<std::mutex> lock(mutex_);
            sample_.speedKph = speed;
            sample_.rpm = rpm;
        }

        std::this_thread::sleep_for(kTickInterval);
    }

    LOGI("Telemetry thread stopped");
}
