#pragma once

#include <android/asset_manager.h>
#include <android/native_window.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// A Vulkan speed-dial renderer, kept deliberately separate from the
// EGL/GLES RenderThread. It started as Phase 3's triangle (comparing
// Vulkan's explicit model against the GLES path), and now draws the live
// needle: ticks, needle and hub, using the same dial shaders as the Unity
// plugin (unity-plugin/shaders, compiled to assets/shaders/dial.*.spv).
//
// Runs its own dedicated thread, same discipline as RenderThread: every
// Vulkan call happens on that one thread, created and torn down there.
// Vertices are built in the vertex shader from gl_VertexIndex, so there's
// no VkBuffer. The needle value goes in as a push constant, so the command
// buffer for each swapchain image is re-recorded every frame.
class VulkanRenderer {
public:
    VulkanRenderer(ANativeWindow* window, AAssetManager* assetManager);
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void start();
    void stop();

    // Called from the UI thread (SurfaceHolder.Callback.surfaceChanged).
    // Only sets a flag; the render thread rebuilds the swapchain itself.
    void notifySurfaceChanged();

    // Latest vehicle speed for the needle, from the app's telemetry thread.
    // Process-wide (an atomic, not a member), so a late telemetry call can't
    // touch a renderer that the UI thread is destroying.
    static void setSpeedKph(float kph);

private:
    // Two frames in flight: the CPU can prepare frame N+1 while the GPU
    // still renders frame N. Each slot has its own acquire semaphore and
    // fence.
    static constexpr int kMaxFramesInFlight = 2;

    void run();
    bool init();
    void teardown();
    bool drawFrame();

    bool createInstance();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSurface();
    bool createSwapchain();
    bool createImageViews();
    bool createRenderPass();
    bool createGraphicsPipeline();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, float needleValue);
    float needleValue();
    bool createSyncObjects();
    bool createRenderFinishedSemaphores();

    // Swapchain-dependent objects: image views, framebuffers, command
    // buffers and per-image semaphores. Rebuilt when the surface changes.
    void destroySwapchainResources();
    bool recreateSwapchain(const char* reason);
    bool swapchainMatchesSurface() const;
    std::vector<uint8_t> loadShaderAsset(const char* path) const;
    VkShaderModule createShaderModule(const std::vector<uint8_t>& code) const;

    ANativeWindow* window_;
    AAssetManager* assetManager_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> surfaceChanged_{false};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    VkSurfaceTransformFlagBitsKHR swapchainTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // Per frame slot: acquire semaphore + fence the CPU waits on before
    // reusing the slot.
    VkSemaphore imageAvailable_[kMaxFramesInFlight] = {};
    VkFence inFlightFences_[kMaxFramesInFlight] = {};
    uint32_t currentFrame_ = 0;

    // Per swapchain image: the "render finished" semaphore that present
    // waits on, and the fence of the frame slot that last used the image
    // (an image can come back while an older slot still renders to it).
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> imagesInFlight_;

    // Needle position 0..1, eased toward the latest speed (render thread only).
    float shownNeedle_ = 0.0f;
    std::chrono::steady_clock::time_point lastNeedleTime_ = std::chrono::steady_clock::now();
    bool loggedLiveSpeed_ = false;
};
