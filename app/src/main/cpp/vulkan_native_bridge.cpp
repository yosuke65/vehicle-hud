#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "vulkan_renderer.h"

#define LOG_TAG "VehicleHud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// JNI surface between VulkanSurfaceView (Kotlin) and VulkanRenderer (C++).
// Same handle-lifetime discipline as HudSurfaceView/RenderThread: the
// jlong handle is a raw VulkanRenderer*, valid strictly between one
// surfaceCreated()/nativeSurfaceCreated() call and the matching
// surfaceDestroyed()/nativeSurfaceDestroyed() call.

extern "C" JNIEXPORT jlong JNICALL
Java_dev_yosukesakurai_vehiclehud_VulkanSurfaceView_nativeSurfaceCreated(
        JNIEnv* env, jobject /* thiz */, jobject surface, jobject assetManager) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGI("ANativeWindow_fromSurface returned null");
        return 0;
    }
    AAssetManager* nativeAssetManager = AAssetManager_fromJava(env, assetManager);

    auto* renderer = new VulkanRenderer(window, nativeAssetManager); // renderer now owns window
    renderer->start();
    return reinterpret_cast<jlong>(renderer);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_yosukesakurai_vehiclehud_VulkanSurfaceView_nativeSurfaceChanged(
        JNIEnv* /* env */, jobject /* thiz */, jlong handle) {
    if (handle == 0) {
        return;
    }
    reinterpret_cast<VulkanRenderer*>(handle)->notifySurfaceChanged();
}

// Called on the telemetry thread. Static (no handle): it only stores the
// speed in an atomic that every VulkanRenderer's render thread reads.
extern "C" JNIEXPORT void JNICALL
Java_dev_yosukesakurai_vehiclehud_VulkanSurfaceView_nativeSetSpeedKph(
        JNIEnv* /* env */, jclass /* clazz */, jfloat speedKph) {
    VulkanRenderer::setSpeedKph(speedKph);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_yosukesakurai_vehiclehud_VulkanSurfaceView_nativeSurfaceDestroyed(
        JNIEnv* /* env */, jobject /* thiz */, jlong handle) {
    if (handle == 0) {
        return;
    }
    auto* renderer = reinterpret_cast<VulkanRenderer*>(handle);
    delete renderer; // ~VulkanRenderer() stops the thread, joins it, then releases the window
}
