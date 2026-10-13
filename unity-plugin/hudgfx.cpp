// Minimal Unity native rendering plugin for the HUD's Unity scene.
//
// Uses Unity's low-level native plugin interface: Unity calls
// UnityPluginLoad() when it loads this library, the plugin registers for
// graphics device events through IUnityGraphics, and C# triggers
// OnRenderEvent() on Unity's render thread every frame through
// GL.IssuePluginEvent(GetRenderEventFunc(), ...).
//
// What it reports (logcat tag "hudgfx"):
// - which graphics API Unity picked on this device (Vulkan or GLES 3)
// - on Vulkan, the GPU and queue family Unity uses, read from Unity's own
//   VkInstance/VkPhysicalDevice through IUnityGraphicsVulkan
// - the thread render events run on, compared with Unity's main thread

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"

#include <android/log.h>
#include <unistd.h>

#include <atomic>

#define LOG_TAG "hudgfx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
UnityGfxRenderer g_renderer = kUnityGfxRendererNull;
std::atomic<int> g_renderEvents{0};

const char* rendererName(UnityGfxRenderer renderer) {
    switch (renderer) {
        case kUnityGfxRendererVulkan: return "Vulkan";
        case kUnityGfxRendererOpenGLES30: return "OpenGL ES 3";
        case kUnityGfxRendererNull: return "Null";
        default: return "Other";
    }
}

void logVulkanDevice() {
    auto* vulkan = g_interfaces->Get<IUnityGraphicsVulkan>();
    if (vulkan == nullptr) {
        LOGI("IUnityGraphicsVulkan not available");
        return;
    }
    UnityVulkanInstance instance = vulkan->Instance();
    // Unity hands over its own loader entry point; look up the one call we
    // need instead of linking libvulkan into the plugin.
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            instance.getInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceProperties"));
    if (getProperties == nullptr) {
        LOGI("Unity Vulkan device: queue family %u (no properties entry point)", instance.queueFamilyIndex);
        return;
    }
    VkPhysicalDeviceProperties properties{};
    getProperties(instance.physicalDevice, &properties);
    LOGI("Unity Vulkan device: %s, API %u.%u.%u, queue family %u", properties.deviceName,
         VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
         VK_API_VERSION_PATCH(properties.apiVersion), instance.queueFamilyIndex);
}

void UNITY_INTERFACE_API onGraphicsDeviceEvent(UnityGfxDeviceEventType type) {
    if (type == kUnityGfxDeviceEventInitialize) {
        g_renderer = g_graphics->GetRenderer();
        LOGI("Graphics device initialized: %s (thread %d)", rendererName(g_renderer), gettid());
        if (g_renderer == kUnityGfxRendererVulkan) {
            logVulkanDevice();
        }
    } else if (type == kUnityGfxDeviceEventShutdown) {
        LOGI("Graphics device shutdown after %d render events", g_renderEvents.load());
        g_renderer = kUnityGfxRendererNull;
    }
}

void UNITY_INTERFACE_API onRenderEvent(int eventId) {
    const int count = ++g_renderEvents;
    if (count == 1 || count % 300 == 0) {
        LOGI("Render event %d #%d on thread %d (%s)", eventId, count, gettid(), rendererName(g_renderer));
    }
}

}  // namespace

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces) {
    g_interfaces = interfaces;
    g_graphics = interfaces->Get<IUnityGraphics>();
    g_graphics->RegisterDeviceEventCallback(onGraphicsDeviceEvent);
    LOGI("UnityPluginLoad (thread %d)", gettid());
    // The device may already exist when the plugin loads.
    onGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload() {
    if (g_graphics != nullptr) {
        g_graphics->UnregisterDeviceEventCallback(onGraphicsDeviceEvent);
    }
}

extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API HudGfx_GetRenderEventFunc() {
    return onRenderEvent;
}

// Called from C# on Unity's main thread, so the log can compare it with the
// render event thread.
extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API HudGfx_CurrentThreadId() {
    return gettid();
}
