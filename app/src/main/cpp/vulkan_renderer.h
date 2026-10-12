#pragma once

#include <android/asset_manager.h>
#include <android/native_window.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <atomic>
#include <thread>
#include <vector>

// A minimal Vulkan triangle renderer, kept deliberately separate from the
// EGL/GLES RenderThread — this is Phase 3 research (comparing Vulkan's
// explicit model against the GLES path already built), not a replacement
// for it, and keeping it isolated means a Vulkan setup mistake can't
// jeopardize the working GLES app.
//
// Runs its own dedicated thread, same discipline as RenderThread: every
// Vulkan call happens on that one thread, created and torn down there.
// Vertex positions are hardcoded in the vertex shader (see
// shaders/triangle.vert) via gl_VertexIndex — deliberate scope control that
// skips VkBuffer/VkDeviceMemory allocation while still exercising the full
// real pipeline: instance, physical/logical device, surface, swapchain,
// render pass, pipeline, command buffers, and present synchronization.
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
};
