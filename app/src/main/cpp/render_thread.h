#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

#include <atomic>
#include <thread>

#include "telemetry_source.h"

// NOTE: this project targets -std=c++20 (see CMakeLists.txt), but std::jthread
// / std::stop_token are not implemented in the NDK's bundled libc++ as of
// NDK 28.2.13676358 — the compiler accepts the C++20 language standard, but
// this specific standard *library* feature isn't there (confirmed by trying
// it: "no type named 'stop_token' in namespace 'std'"). Compiler support and
// standard library support are separate things, and this is a real example
// of that gap on a real embedded/mobile toolchain — so this still uses the
// manual std::thread + atomic<bool> stop-flag pattern, not std::jthread.

// Owns exactly one native render thread for the lifetime of one Android
// Surface, plus the independent telemetry worker thread that feeds it.
// All EGL/GL calls — context creation, drawing, and teardown — happen on
// the render thread only; nothing here is ever touched from the Java/UI
// thread or from the telemetry thread. The owning ANativeWindow is
// released in the destructor, after the render thread has been joined.
class RenderThread {
public:
    // Takes ownership of `window` (caller must not release it separately).
    explicit RenderThread(ANativeWindow* window);
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start();
    // Signals the loop to exit and blocks until the thread has torn down
    // EGL and returned. Safe to call more than once.
    void stop();

    // Forwards to the owned TelemetrySource — called from the JNI bridge
    // on the Kotlin-side AIDL-polling thread, never from the render
    // thread itself.
    void setExternalTelemetry(float speedKph, float rpm) {
        telemetry_.setExternal(speedKph, rpm);
    }

private:
    void run();
    bool initEgl();
    void teardownEgl();
    void renderFrame(const TelemetrySample& sample);

    ANativeWindow* window_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    TelemetrySource telemetry_;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;

    GLuint program_ = 0;
    GLint positionHandle_ = -1;
    GLint angleHandle_ = -1;

    // Most recent per-frame timing split, written by renderFrame() and
    // read back by run() for the rolling profiling average — both calls
    // happen on the render thread only, so no synchronization is needed.
    long long lastDrawMicros_ = 0;
    long long lastSwapMicros_ = 0;
};
