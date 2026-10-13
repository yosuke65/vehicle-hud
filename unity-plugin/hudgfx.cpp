// Minimal Unity native rendering plugin for the HUD's Unity scene.
//
// Uses Unity's low-level native plugin interface: Unity calls
// UnityPluginLoad() when it loads this library, the plugin registers for
// graphics device events through IUnityGraphics, and C# triggers
// OnRenderEvent() on Unity's render thread through
// GL.IssuePluginEvent(GetRenderEventFunc(), eventId).
//
// What it does:
// - logs which graphics API Unity picked on this device (Vulkan or GLES 3)
//   and, on Vulkan, the GPU Unity uses, read from Unity's own
//   VkInstance/VkPhysicalDevice through IUnityGraphicsVulkan
// - logs the thread render events run on, compared with Unity's main thread
// - draws a speed dial (ticks, needle, hub) in two places:
//   - event 1, kOverlayEventId: on top of Unity's frame, inside Unity's
//     render pass, with Unity's own GL context or Vulkan command buffer
//   - event 2, kTextureEventId: into a Unity RenderTexture that the scene
//     shows on a panel. On Vulkan, AccessTexture hands over the VkImage in
//     the GENERAL layout with the barrier done, and a compute shader writes
//     the dial into it as a storage image: Unity records plugin events into
//     secondary command buffers, where a render pass can't begin. On GLES it
//     draws the vertex dial through an FBO.
// - the needle follows the app's vehicle speed: Kotlin pushes it through JNI
//   (UnityDialBridge.nativeSetSpeedKph) from its telemetry thread, and the
//   render thread reads it from an atomic.
// The dial layout lives in shaders/dial_body.glsl and is shared by every
// path; the vertex paths build each vertex from its index, so there's no
// vertex buffer, and the compute path draws the same layout per pixel.

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#define LOG_TAG "hudgfx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kOverlayEventId = 1;
constexpr int kTextureEventId = 2;
constexpr int kDialVertexCount = 7 * 6 + 3 + 6;  // matches kVertexCount in dial_body.glsl
// Same full-scale speed as the native HUD's gauge (render_thread.cpp).
constexpr float kMaxSpeedKph = 180.0f;
// Smoothing time constant: speed arrives at about 10 Hz, frames at 30-60 Hz.
constexpr float kNeedleTauSeconds = 0.12f;
// Overlay: a small dial in the top-left corner of the screen.
constexpr float kOverlayCenterX = -0.6f;
constexpr float kOverlayCenterY = 0.8f;
constexpr float kOverlayRadius = 0.13f;
// Texture: the dial fills the texture.
constexpr float kTextureRadius = 0.9f;
constexpr uint32_t kComputeGroupSize = 8;  // local_size in dial.comp
constexpr float kTextureClear[4] = {0.06f, 0.08f, 0.10f, 1.0f};

const uint32_t kDialVertSpv[] =
#include "dial.vert.spv.inc"
;
const uint32_t kDialFragSpv[] =
#include "dial.frag.spv.inc"
;
const uint32_t kDialCompSpv[] =
#include "dial.comp.spv.inc"
;
const char* const kDialBodyGlsl =
#include "dial_body.glsl.inc"
;

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
UnityGfxRenderer g_renderer = kUnityGfxRendererNull;
std::atomic<int> g_renderEvents{0};

// Written from other threads, read on the render thread.
std::atomic<float> g_speedKph{0.0f};           // Kotlin telemetry thread, via JNI
std::atomic<int> g_screenWidth{0};             // C#, Unity's main thread
std::atomic<int> g_screenHeight{0};
std::atomic<void*> g_dialTexture{nullptr};     // C#: RenderTexture.GetNativeTexturePtr()
std::atomic<int> g_dialTextureWidth{0};
std::atomic<int> g_dialTextureHeight{0};

const char* rendererName(UnityGfxRenderer renderer) {
    switch (renderer) {
        case kUnityGfxRendererVulkan: return "Vulkan";
        case kUnityGfxRendererOpenGLES30: return "OpenGL ES 3";
        case kUnityGfxRendererNull: return "Null";
        default: return "Other";
    }
}

struct DialParams {
    float centerX, centerY;
    float radius;
    float aspect;
    float value;
    float yFlip;
};

// Needle position 0..1, eased toward the latest speed so 10 Hz samples don't
// make the needle step. Only the render thread calls this.
float needleValue() {
    using Clock = std::chrono::steady_clock;
    static float shown = 0.0f;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    const float target = std::clamp(g_speedKph.load() / kMaxSpeedKph, 0.0f, 1.0f);
    shown += (target - shown) * (1.0f - std::exp(-dt / kNeedleTauSeconds));
    return shown;
}

DialParams overlayParams(int width, int height, float value, float yFlip) {
    return {kOverlayCenterX, kOverlayCenterY, kOverlayRadius,
            static_cast<float>(width) / static_cast<float>(height), value, yFlip};
}

DialParams textureParams(int width, int height, float value, float yFlip) {
    return {0.0f, 0.0f, kTextureRadius, static_cast<float>(width) / static_cast<float>(height), value, yFlip};
}

// ---------------------------------------------------------------- GLES path

// Created lazily on the render thread: UnityPluginLoad and the device
// initialize event run on Unity's main thread, which doesn't own the context.
struct GlesDial {
    GLuint program = 0;
    GLuint vao = 0;
    GLint uCenter = -1, uRadius = -1, uAspect = -1, uValue = -1, uYFlip = -1;
    GLuint fbo = 0;
    GLuint fboTexture = 0;
    bool failed = false;
    bool loggedOverlay = false;
    bool loggedTexture = false;
} g_gles;

GLuint compileGlesShader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("GLES shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool createGlesDial() {
    const std::string vertexSource = std::string(
            "#version 300 es\n"
            "uniform vec2 uCenter;\n"
            "uniform float uRadius;\n"
            "uniform float uAspect;\n"
            "uniform float uValue;\n"
            "uniform float uYFlip;\n"
            "out vec3 vColor;\n") +
            kDialBodyGlsl +
            "void main() {\n"
            "    vec3 color;\n"
            "    vec2 p = dialPoint(gl_VertexID, uValue, color);\n"
            "    vColor = color;\n"
            "    gl_Position = vec4(uCenter.x + p.x * uRadius / uAspect,\n"
            "                       (uCenter.y + p.y * uRadius) * uYFlip, 0.0, 1.0);\n"
            "}\n";
    const std::string fragmentSource =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec3 vColor;\n"
            "out vec4 outColor;\n"
            "void main() { outColor = vec4(vColor, 1.0); }\n";

    GLuint vs = compileGlesShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileGlesShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vs == 0 || fs == 0) return false;
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("GLES program link failed: %s", log);
        glDeleteProgram(program);
        return false;
    }
    g_gles.program = program;
    g_gles.uCenter = glGetUniformLocation(program, "uCenter");
    g_gles.uRadius = glGetUniformLocation(program, "uRadius");
    g_gles.uAspect = glGetUniformLocation(program, "uAspect");
    g_gles.uValue = glGetUniformLocation(program, "uValue");
    g_gles.uYFlip = glGetUniformLocation(program, "uYFlip");
    // An empty VAO of our own, so Unity's vertex attribute setup is untouched.
    glGenVertexArrays(1, &g_gles.vao);
    return true;
}

bool ensureGlesDial() {
    if (g_gles.failed) return false;
    if (g_gles.program == 0 && !createGlesDial()) {
        g_gles.failed = true;
        return false;
    }
    return true;
}

// Unity caches GL state, so everything the dial draw changes is saved here
// and put back afterwards.
struct SavedGlState {
    GLint program, vao, framebuffer, viewport[4];
    GLfloat clearColor[4];
    GLboolean colorMask[4];
    GLboolean depthTest, cullFace, blend, scissor;

    void save() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        cullFace = glIsEnabled(GL_CULL_FACE);
        blend = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
    }

    void restore() const {
        glBindVertexArray(static_cast<GLuint>(vao));
        glUseProgram(static_cast<GLuint>(program));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        if (depthTest) glEnable(GL_DEPTH_TEST);
        if (cullFace) glEnable(GL_CULL_FACE);
        if (blend) glEnable(GL_BLEND);
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }
};

void issueGlesDial(const DialParams& params, int width, int height) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width, height);
    glUseProgram(g_gles.program);
    glUniform2f(g_gles.uCenter, params.centerX, params.centerY);
    glUniform1f(g_gles.uRadius, params.radius);
    glUniform1f(g_gles.uAspect, params.aspect);
    glUniform1f(g_gles.uValue, params.value);
    glUniform1f(g_gles.uYFlip, params.yFlip);
    glBindVertexArray(g_gles.vao);
    glDrawArrays(GL_TRIANGLES, 0, kDialVertexCount);
}

void drawGlesOverlay(float value) {
    const int width = g_screenWidth.load();
    const int height = g_screenHeight.load();
    if (width <= 0 || height <= 0 || !ensureGlesDial()) return;
    SavedGlState saved;
    saved.save();
    issueGlesDial(overlayParams(width, height, value, 1.0f), width, height);
    saved.restore();
    if (!g_gles.loggedOverlay) {
        g_gles.loggedOverlay = true;
        LOGI("GLES overlay dial drawn: %d vertices, %dx%d, glGetError 0x%x", kDialVertexCount, width, height,
             glGetError());
    }
}

void drawGlesTexture(float value) {
    // On GLES, GetNativeTexturePtr() is the GL texture name.
    const auto texture = static_cast<GLuint>(reinterpret_cast<uintptr_t>(g_dialTexture.load()));
    const int width = g_dialTextureWidth.load();
    const int height = g_dialTextureHeight.load();
    if (texture == 0 || width <= 0 || height <= 0 || !ensureGlesDial()) return;
    SavedGlState saved;
    saved.save();
    if (g_gles.fbo == 0 || g_gles.fboTexture != texture) {
        if (g_gles.fbo == 0) glGenFramebuffers(1, &g_gles.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gles.fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("GLES dial FBO incomplete: 0x%x", status);
            saved.restore();
            g_dialTexture.store(nullptr);
            return;
        }
        g_gles.fboTexture = texture;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gles.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(kTextureClear[0], kTextureClear[1], kTextureClear[2], kTextureClear[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    issueGlesDial(textureParams(width, height, value, 1.0f), width, height);
    saved.restore();
    if (!g_gles.loggedTexture) {
        g_gles.loggedTexture = true;
        LOGI("GLES texture dial drawn into texture %u through FBO %u, %dx%d, glGetError 0x%x", texture,
             g_gles.fbo, width, height, glGetError());
    }
}

// -------------------------------------------------------------- Vulkan path

struct VulkanDial {
    IUnityGraphicsVulkan* unity = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCmdSetViewport cmdSetViewport = nullptr;
    PFN_vkCmdSetScissor cmdSetScissor = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // A pipeline is tied to a render pass (and compatible ones), and Unity
    // may draw the backbuffer with more than one, so cache one per pass.
    std::map<std::pair<VkRenderPass, int>, VkPipeline> pipelines;
    // Compute path for drawing into a Unity texture: one storage-image
    // descriptor, pointed at the current texture's view.
    VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkImage boundImage = VK_NULL_HANDLE;
    std::map<VkImage, VkImageView> textureViews;
    bool failed = false;
    bool loggedOverlay = false;
    bool loggedTexture = false;
} g_vk;

template <typename T>
bool loadDeviceProc(PFN_vkGetDeviceProcAddr getDeviceProcAddr, const char* name, T* out) {
    *out = reinterpret_cast<T>(getDeviceProcAddr(g_vk.device, name));
    if (*out == nullptr) LOGE("Vulkan: %s not found", name);
    return *out != nullptr;
}

bool initVulkanDial(IUnityGraphicsVulkan* vulkan) {
    UnityVulkanInstance instance = vulkan->Instance();
    g_vk.unity = vulkan;
    g_vk.device = instance.device;
    // Unity hands over its own loader entry point; look up what we need
    // instead of linking libvulkan into the plugin.
    auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr"));
    if (getDeviceProcAddr == nullptr) return false;
    bool ok = loadDeviceProc(getDeviceProcAddr, "vkCreateShaderModule", &g_vk.createShaderModule) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyShaderModule", &g_vk.destroyShaderModule) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreatePipelineLayout", &g_vk.createPipelineLayout) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyPipelineLayout", &g_vk.destroyPipelineLayout) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreateGraphicsPipelines", &g_vk.createGraphicsPipelines) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyPipeline", &g_vk.destroyPipeline) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreateComputePipelines", &g_vk.createComputePipelines) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreateImageView", &g_vk.createImageView) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyImageView", &g_vk.destroyImageView) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreateDescriptorSetLayout", &g_vk.createDescriptorSetLayout) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyDescriptorSetLayout", &g_vk.destroyDescriptorSetLayout) &&
              loadDeviceProc(getDeviceProcAddr, "vkCreateDescriptorPool", &g_vk.createDescriptorPool) &&
              loadDeviceProc(getDeviceProcAddr, "vkDestroyDescriptorPool", &g_vk.destroyDescriptorPool) &&
              loadDeviceProc(getDeviceProcAddr, "vkAllocateDescriptorSets", &g_vk.allocateDescriptorSets) &&
              loadDeviceProc(getDeviceProcAddr, "vkUpdateDescriptorSets", &g_vk.updateDescriptorSets) &&
              loadDeviceProc(getDeviceProcAddr, "vkDeviceWaitIdle", &g_vk.deviceWaitIdle) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdBindDescriptorSets", &g_vk.cmdBindDescriptorSets) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdDispatch", &g_vk.cmdDispatch) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdBindPipeline", &g_vk.cmdBindPipeline) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdPushConstants", &g_vk.cmdPushConstants) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdSetViewport", &g_vk.cmdSetViewport) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdSetScissor", &g_vk.cmdSetScissor) &&
              loadDeviceProc(getDeviceProcAddr, "vkCmdDraw", &g_vk.cmdDraw);
    if (!ok) return false;

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(DialParams)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (g_vk.createPipelineLayout(g_vk.device, &layoutInfo, nullptr, &g_vk.layout) != VK_SUCCESS) return false;

    // Overlay: Unity must be inside a render pass, so we draw straight into
    // its current target. ModifiesCommandBuffersState because we bind our
    // own pipeline, so Unity re-binds its state afterwards.
    UnityVulkanPluginEventConfig overlay{};
    overlay.renderPassPrecondition = kUnityVulkanRenderPass_EnsureInside;
    overlay.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
    overlay.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission |
                    kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState;
    vulkan->ConfigureEvent(kOverlayEventId, &overlay);
    // Texture: Unity must be outside a render pass, because a dispatch can't
    // run inside one.
    UnityVulkanPluginEventConfig texture = overlay;
    texture.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
    vulkan->ConfigureEvent(kTextureEventId, &texture);
    return true;
}

VkShaderModule createVulkanShader(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (g_vk.createShaderModule(g_vk.device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

VkPipeline vulkanPipelineFor(VkRenderPass renderPass, int subpass) {
    const auto key = std::make_pair(renderPass, subpass);
    auto found = g_vk.pipelines.find(key);
    if (found != g_vk.pipelines.end()) return found->second;

    VkShaderModule vert = createVulkanShader(kDialVertSpv, sizeof(kDialVertSpv));
    VkShaderModule frag = createVulkanShader(kDialFragSpv, sizeof(kDialFragSpv));
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        // The Android quality level has MSAA off, so Unity's backbuffer pass
        // is single-sampled.
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // Unity's pass may have a depth attachment; the dial ignores it.
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = g_vk.layout;
        info.renderPass = renderPass;
        info.subpass = static_cast<uint32_t>(subpass);
        const VkResult result =
                g_vk.createGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            LOGE("vkCreateGraphicsPipelines failed: %d", result);
            pipeline = VK_NULL_HANDLE;
        } else {
            LOGI("Vulkan dial pipeline created for render pass %p, subpass %d",
                 reinterpret_cast<void*>(renderPass), subpass);
        }
    }
    if (vert != VK_NULL_HANDLE) g_vk.destroyShaderModule(g_vk.device, vert, nullptr);
    if (frag != VK_NULL_HANDLE) g_vk.destroyShaderModule(g_vk.device, frag, nullptr);
    if (pipeline != VK_NULL_HANDLE) g_vk.pipelines[key] = pipeline;
    return pipeline;
}

void recordVulkanDial(VkCommandBuffer commandBuffer, VkPipeline pipeline, const DialParams& params, uint32_t width,
                      uint32_t height) {
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    g_vk.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g_vk.cmdSetViewport(commandBuffer, 0, 1, &viewport);
    g_vk.cmdSetScissor(commandBuffer, 0, 1, &scissor);
    g_vk.cmdPushConstants(commandBuffer, g_vk.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(params), &params);
    g_vk.cmdDraw(commandBuffer, kDialVertexCount, 1, 0, 0);
}

void drawVulkanOverlay(float value) {
    if (g_vk.failed || g_vk.unity == nullptr) return;
    const int width = g_screenWidth.load();
    const int height = g_screenHeight.load();
    if (width <= 0 || height <= 0) return;

    UnityVulkanRecordingState state{};
    if (!g_vk.unity->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare)) return;
    if (state.renderPass == VK_NULL_HANDLE || state.subPassIndex < 0) return;
    VkPipeline pipeline = vulkanPipelineFor(state.renderPass, state.subPassIndex);
    if (pipeline == VK_NULL_HANDLE) {
        g_vk.failed = true;
        return;
    }
    // Vulkan's NDC y points down, hence yFlip -1.
    recordVulkanDial(state.commandBuffer, pipeline, overlayParams(width, height, value, -1.0f),
                     static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (!g_vk.loggedOverlay) {
        g_vk.loggedOverlay = true;
        LOGI("Vulkan overlay dial recorded into Unity's command buffer (level %d, frame %llu), %dx%d",
             state.commandBufferLevel, state.currentFrameNumber, width, height);
    }
}

struct ComputePush {
    float value;
    float radius;
    float yFlip;
};

bool createVulkanCompute() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &binding;
    if (g_vk.createDescriptorSetLayout(g_vk.device, &setLayoutInfo, nullptr, &g_vk.computeSetLayout) != VK_SUCCESS) {
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_vk.computeSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (g_vk.createPipelineLayout(g_vk.device, &layoutInfo, nullptr, &g_vk.computeLayout) != VK_SUCCESS) return false;

    VkShaderModule comp = createVulkanShader(kDialCompSpv, sizeof(kDialCompSpv));
    if (comp == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = comp;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = g_vk.computeLayout;
    const VkResult result =
            g_vk.createComputePipelines(g_vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_vk.computePipeline);
    g_vk.destroyShaderModule(g_vk.device, comp, nullptr);
    if (result != VK_SUCCESS) return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (g_vk.createDescriptorPool(g_vk.device, &poolInfo, nullptr, &g_vk.descriptorPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = g_vk.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &g_vk.computeSetLayout;
    if (g_vk.allocateDescriptorSets(g_vk.device, &allocInfo, &g_vk.descriptorSet) != VK_SUCCESS) return false;
    LOGI("Vulkan compute dial pipeline created");
    return true;
}

// Points the storage-image descriptor at this texture. Only done when the
// texture changes, so a set the GPU may still be reading isn't rewritten
// every frame.
bool bindVulkanTexture(const UnityVulkanImage& image) {
    if (g_vk.boundImage == image.image) return true;
    VkImageView view = VK_NULL_HANDLE;
    auto found = g_vk.textureViews.find(image.image);
    if (found != g_vk.textureViews.end()) {
        view = found->second;
    } else {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = image.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (g_vk.createImageView(g_vk.device, &viewInfo, nullptr, &view) != VK_SUCCESS) return false;
        g_vk.textureViews[image.image] = view;
    }
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = g_vk.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;
    g_vk.updateDescriptorSets(g_vk.device, 1, &write, 0, nullptr);
    g_vk.boundImage = image.image;
    return true;
}

void drawVulkanTexture(float value) {
    void* texture = g_dialTexture.load();
    if (g_vk.failed || g_vk.unity == nullptr || texture == nullptr) return;
    auto disable = [](const char* why) {
        LOGE("Texture dial disabled: %s", why);
        g_dialTexture.store(nullptr);
    };
    if (g_vk.computePipeline == VK_NULL_HANDLE && !createVulkanCompute()) return disable("compute setup failed");

    // AccessTexture first: it records the layout transition and barrier into
    // Unity's command buffer, which invalidates any recording state fetched
    // before it. GENERAL is the layout a storage image is written in.
    UnityVulkanImage image{};
    const VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    if (!g_vk.unity->AccessTexture(texture, &subresource, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   kUnityVulkanResourceAccess_PipelineBarrier, &image)) {
        return disable("AccessTexture failed");
    }
    if ((image.usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0) {
        return disable("texture has no storage usage (RenderTexture.enableRandomWrite off?)");
    }
    UnityVulkanRecordingState state{};
    if (!g_vk.unity->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare)) return;
    if (!bindVulkanTexture(image)) return disable("image view or descriptor update failed");

    // Unity samples this texture with row 0 at the bottom, so draw it with
    // y flipped (seen on device: with 1.0 the dial came out upside down).
    const ComputePush push{value, kTextureRadius, -1.0f};
    const uint32_t width = image.extent.width;
    const uint32_t height = image.extent.height;
    g_vk.cmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk.computePipeline);
    g_vk.cmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk.computeLayout, 0, 1,
                               &g_vk.descriptorSet, 0, nullptr);
    g_vk.cmdPushConstants(state.commandBuffer, g_vk.computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                          &push);
    g_vk.cmdDispatch(state.commandBuffer, (width + kComputeGroupSize - 1) / kComputeGroupSize,
                     (height + kComputeGroupSize - 1) / kComputeGroupSize, 1);

    if (!g_vk.loggedTexture) {
        g_vk.loggedTexture = true;
        LOGI("Vulkan texture dial: AccessTexture gave VkImage %p, format %d, usage 0x%x, %ux%u, layout %d; "
             "dispatched %ux%u groups into Unity's command buffer (level %d, frame %llu)",
             reinterpret_cast<void*>(image.image), image.format, image.usage, width, height, image.layout,
             (width + kComputeGroupSize - 1) / kComputeGroupSize, (height + kComputeGroupSize - 1) / kComputeGroupSize,
             state.commandBufferLevel, state.currentFrameNumber);
    }
}

void destroyVulkanDial() {
    if (g_vk.device == VK_NULL_HANDLE) return;
    g_vk.deviceWaitIdle(g_vk.device);
    for (auto& entry : g_vk.textureViews) g_vk.destroyImageView(g_vk.device, entry.second, nullptr);
    if (g_vk.descriptorPool != VK_NULL_HANDLE) g_vk.destroyDescriptorPool(g_vk.device, g_vk.descriptorPool, nullptr);
    if (g_vk.computePipeline != VK_NULL_HANDLE) g_vk.destroyPipeline(g_vk.device, g_vk.computePipeline, nullptr);
    if (g_vk.computeLayout != VK_NULL_HANDLE) g_vk.destroyPipelineLayout(g_vk.device, g_vk.computeLayout, nullptr);
    if (g_vk.computeSetLayout != VK_NULL_HANDLE) {
        g_vk.destroyDescriptorSetLayout(g_vk.device, g_vk.computeSetLayout, nullptr);
    }
    for (auto& entry : g_vk.pipelines) g_vk.destroyPipeline(g_vk.device, entry.second, nullptr);
    if (g_vk.layout != VK_NULL_HANDLE) g_vk.destroyPipelineLayout(g_vk.device, g_vk.layout, nullptr);
    g_vk = VulkanDial{};
}

// ------------------------------------------------------------ Unity hooks

void logVulkanDevice(IUnityGraphicsVulkan* vulkan) {
    UnityVulkanInstance instance = vulkan->Instance();
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
            auto* vulkan = g_interfaces->Get<IUnityGraphicsVulkan>();
            if (vulkan == nullptr) {
                LOGI("IUnityGraphicsVulkan not available");
                return;
            }
            logVulkanDevice(vulkan);
            if (!initVulkanDial(vulkan)) {
                LOGE("Vulkan dial setup failed; drawing disabled");
                g_vk.failed = true;
            }
        }
    } else if (type == kUnityGfxDeviceEventShutdown) {
        LOGI("Graphics device shutdown after %d render events", g_renderEvents.load());
        if (g_renderer == kUnityGfxRendererVulkan) destroyVulkanDial();
        // GL objects go with Unity's context; this callback may not be on the
        // thread that owns it, so they aren't deleted here.
        g_gles = GlesDial{};
        g_renderer = kUnityGfxRendererNull;
    }
}

void UNITY_INTERFACE_API onRenderEvent(int eventId) {
    const int count = ++g_renderEvents;
    if (count == 1 || count % 600 == 0) {
        LOGI("Render event %d #%d on thread %d (%s), speed %.1f km/h", eventId, count, gettid(),
             rendererName(g_renderer), g_speedKph.load());
    }
    const float value = needleValue();
    const bool gles = g_renderer == kUnityGfxRendererOpenGLES30;
    const bool vulkan = g_renderer == kUnityGfxRendererVulkan;
    if (eventId == kOverlayEventId) {
        if (gles) drawGlesOverlay(value);
        if (vulkan) drawVulkanOverlay(value);
    } else if (eventId == kTextureEventId) {
        if (gles) drawGlesTexture(value);
        if (vulkan) drawVulkanTexture(value);
    }
}

}  // namespace

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces) {
    g_interfaces = interfaces;
    g_graphics = interfaces->Get<IUnityGraphics>();
    g_graphics->RegisterDeviceEventCallback(onGraphicsDeviceEvent);
    LOGI("UnityPluginLoad (thread %d), speed state at %p", gettid(), static_cast<void*>(&g_speedKph));
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

// Called from C# on Unity's main thread each frame.
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API HudGfx_SetScreenSize(int width, int height) {
    g_screenWidth.store(width);
    g_screenHeight.store(height);
}

// Called from C# once the RenderTexture exists: GetNativeTexturePtr() is a GL
// texture name on GLES and a VkImage* on Vulkan.
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API HudGfx_SetDialTexture(void* texture, int width,
                                                                                    int height) {
    g_dialTextureWidth.store(width);
    g_dialTextureHeight.store(height);
    g_dialTexture.store(texture);
    LOGI("Dial texture set: %p, %dx%d", texture, width, height);
}

// Called by the host app's telemetry thread (TelemetryRouter ->
// UnityDialBridge). Same process and same library instance as Unity's.
extern "C" JNIEXPORT void JNICALL Java_dev_yosukesakurai_vehiclehud_UnityDialBridge_nativeSetSpeedKph(
        JNIEnv*, jobject, jfloat kph) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        LOGI("First speed from the app: %.1f km/h, speed state at %p (thread %d)", kph,
             static_cast<void*>(&g_speedKph), gettid());
    }
    g_speedKph.store(kph);
}
