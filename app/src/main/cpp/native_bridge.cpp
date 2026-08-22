#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "render_thread.h"

#define LOG_TAG "VehicleHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// JNI surface between HudSurfaceView (Kotlin) and RenderThread (C++). The
// jlong handle is a raw RenderThread* — valid strictly between one
// surfaceCreated()/nativeSurfaceCreated() call and the matching
// surfaceDestroyed()/nativeSurfaceDestroyed() call.

extern "C" JNIEXPORT jlong JNICALL
Java_dev_yosukesakurai_vehiclehud_HudSurfaceView_nativeSurfaceCreated(
        JNIEnv* env, jobject /* thiz */, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGI("ANativeWindow_fromSurface returned null");
        return 0;
    }

    auto* renderThread = new RenderThread(window); // RenderThread now owns window
    renderThread->start();
    return reinterpret_cast<jlong>(renderThread);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_yosukesakurai_vehiclehud_HudSurfaceView_nativeSurfaceDestroyed(
        JNIEnv* /* env */, jobject /* thiz */, jlong handle) {
    if (handle == 0) {
        return;
    }
    auto* renderThread = reinterpret_cast<RenderThread*>(handle);
    delete renderThread; // ~RenderThread() stops the thread, joins it, then releases the window
}

// Called from MainActivity's AIDL-polling thread (see
// TelemetryAidlPoll in MainActivity.kt) — a plain JNI down-call from an
// existing Kotlin thread, so no JavaVM::AttachCurrentThread is needed.
extern "C" JNIEXPORT void JNICALL
Java_dev_yosukesakurai_vehiclehud_HudSurfaceView_nativeSetTelemetry(
        JNIEnv* /* env */, jobject /* thiz */, jlong handle, jfloat speedKph, jfloat rpm) {
    if (handle == 0) {
        return;
    }
    auto* renderThread = reinterpret_cast<RenderThread*>(handle);
    renderThread->setExternalTelemetry(speedKph, rpm);
}
