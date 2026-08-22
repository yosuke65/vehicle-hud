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

private:
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
    bool createCommandPoolAndBuffers();
    bool createSyncObjects();
    std::vector<uint8_t> loadShaderAsset(const char* path) const;
    VkShaderModule createShaderModule(const std::vector<uint8_t>& code) const;

    ANativeWindow* window_;
    AAssetManager* assetManager_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // Single-frame-in-flight: simplest correct sync for a static scene at
    // this scope. A real app would double/triple-buffer these.
    VkSemaphore imageAvailableSemaphore_ = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore_ = VK_NULL_HANDLE;
    VkFence inFlightFence_ = VK_NULL_HANDLE;
};
