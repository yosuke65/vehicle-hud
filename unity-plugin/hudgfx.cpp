// Minimal Unity native rendering plugin for the HUD's Unity scene.
//
// Uses Unity's low-level native plugin interface: Unity calls
// UnityPluginLoad() when it loads this library, the plugin registers for
// graphics device events through IUnityGraphics, and C# triggers
// OnRenderEvent() on Unity's render thread every frame through
// GL.IssuePluginEvent(GetRenderEventFunc(), ...).
//
// What it does:
// - logs which graphics API Unity picked on this device (Vulkan or GLES 3)
//   and, on Vulkan, the GPU Unity uses, read from Unity's own
//   VkInstance/VkPhysicalDevice through IUnityGraphicsVulkan
// - logs the thread render events run on, compared with Unity's main thread
// - draws a speed dial (ticks, needle, hub) on top of Unity's frame, with
//   Unity's own GL context or Unity's own Vulkan command buffer. C# sets the
//   needle value on the main thread; the render thread reads it.
// The dial geometry lives in shaders/dial_body.glsl and is shared by both
// paths; each vertex is built from its index, so there's no vertex buffer.

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#define LOG_TAG "hudgfx"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kDrawEventId = 1;
constexpr int kDialVertexCount = 7 * 6 + 3 + 6;  // matches kVertexCount in dial_body.glsl
constexpr float kDialCenterX = 0.0f;
constexpr float kDialCenterY = 0.45f;  // upper half of the screen
constexpr float kDialRadius = 0.22f;

const uint32_t kDialVertSpv[] =
#include "dial.vert.spv.inc"
;
const uint32_t kDialFragSpv[] =
#include "dial.frag.spv.inc"
;
const char* const kDialBodyGlsl =
#include "dial_body.glsl.inc"
;

IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
UnityGfxRenderer g_renderer = kUnityGfxRendererNull;
std::atomic<int> g_renderEvents{0};

// Written by C# on Unity's main thread, read on the render thread.
std::atomic<float> g_needleValue{0.0f};
std::atomic<int> g_targetWidth{0};
std::atomic<int> g_targetHeight{0};

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

bool currentDialParams(float yFlip, DialParams* out, int* width, int* height) {
    *width = g_targetWidth.load();
    *height = g_targetHeight.load();
    if (*width <= 0 || *height <= 0) return false;
    *out = {kDialCenterX, kDialCenterY, kDialRadius,
            static_cast<float>(*width) / static_cast<float>(*height), g_needleValue.load(), yFlip};
    return true;
}

// ---------------------------------------------------------------- GLES path

// Created lazily on the render thread: UnityPluginLoad and the device
// initialize event run on Unity's main thread, which doesn't own the context.
struct GlesDial {
    GLuint program = 0;
    GLuint vao = 0;
    GLint uCenter = -1, uRadius = -1, uAspect = -1, uValue = -1, uYFlip = -1;
    bool failed = false;
    bool loggedFirstDraw = false;
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

void drawGlesDial() {
    if (g_gles.failed) return;
    if (g_gles.program == 0 && !createGlesDial()) {
        g_gles.failed = true;
        return;
    }
    DialParams params;
    int width, height;
    if (!currentDialParams(1.0f, &params, &width, &height)) return;

    // Unity caches GL state, so put back everything this draw changes.
    GLint prevProgram, prevVao, prevViewport[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glUseProgram(g_gles.program);
    glUniform2f(g_gles.uCenter, params.centerX, params.centerY);
    glUniform1f(g_gles.uRadius, params.radius);
    glUniform1f(g_gles.uAspect, params.aspect);
    glUniform1f(g_gles.uValue, params.value);
    glUniform1f(g_gles.uYFlip, params.yFlip);
    glBindVertexArray(g_gles.vao);
    glDrawArrays(GL_TRIANGLES, 0, kDialVertexCount);

    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProgram));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (depthTest) glEnable(GL_DEPTH_TEST);
    if (cullFace) glEnable(GL_CULL_FACE);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);

    if (!g_gles.loggedFirstDraw) {
        g_gles.loggedFirstDraw = true;
        LOGI("GLES dial drawn: %d vertices, %dx%d, glGetError 0x%x", kDialVertexCount, width, height,
             glGetError());
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
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCmdSetViewport cmdSetViewport = nullptr;
    PFN_vkCmdSetScissor cmdSetScissor = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // A pipeline is tied to a render pass (and compatible ones), and Unity
    // may draw the backbuffer with more than one, so cache one per pass.
    std::map<std::pair<VkRenderPass, int>, VkPipeline> pipelines;
    bool failed = false;
    bool loggedFirstDraw = false;
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
              loadDeviceProc(getDeviceProcAddr, "vkDeviceWaitIdle", &g_vk.deviceWaitIdle) &&
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

    // Ask Unity to be inside a render pass when our event runs, so we can
    // draw straight into its current target, and to reset its own command
    // buffer state afterwards because we bind our own pipeline.
    UnityVulkanPluginEventConfig config{};
    config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureInside;
    config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
    config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission |
                   kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState;
    vulkan->ConfigureEvent(kDrawEventId, &config);
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
            LOGI("Vulkan dial pipeline created for Unity render pass %p, subpass %d",
                 reinterpret_cast<void*>(renderPass), subpass);
        }
    }
    if (vert != VK_NULL_HANDLE) g_vk.destroyShaderModule(g_vk.device, vert, nullptr);
    if (frag != VK_NULL_HANDLE) g_vk.destroyShaderModule(g_vk.device, frag, nullptr);
    if (pipeline != VK_NULL_HANDLE) g_vk.pipelines[key] = pipeline;
    return pipeline;
}

void drawVulkanDial() {
    if (g_vk.failed || g_vk.unity == nullptr) return;
    DialParams params;
    int width, height;
    if (!currentDialParams(-1.0f, &params, &width, &height)) return;

    UnityVulkanRecordingState state{};
    if (!g_vk.unity->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare)) return;
    if (state.renderPass == VK_NULL_HANDLE || state.subPassIndex < 0) return;
    VkPipeline pipeline = vulkanPipelineFor(state.renderPass, state.subPassIndex);
    if (pipeline == VK_NULL_HANDLE) {
        g_vk.failed = true;
        return;
    }

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
    g_vk.cmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g_vk.cmdSetViewport(state.commandBuffer, 0, 1, &viewport);
    g_vk.cmdSetScissor(state.commandBuffer, 0, 1, &scissor);
    g_vk.cmdPushConstants(state.commandBuffer, g_vk.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(params),
                          &params);
    g_vk.cmdDraw(state.commandBuffer, kDialVertexCount, 1, 0, 0);

    if (!g_vk.loggedFirstDraw) {
        g_vk.loggedFirstDraw = true;
        LOGI("Vulkan dial recorded into Unity's command buffer (level %d, frame %llu), %dx%d",
             state.commandBufferLevel, state.currentFrameNumber, width, height);
    }
}

void destroyVulkanDial() {
    if (g_vk.device == VK_NULL_HANDLE) return;
    g_vk.deviceWaitIdle(g_vk.device);
    for (auto& entry : g_vk.pipelines) g_vk.destroyPipeline(g_vk.device, entry.second, nullptr);
    g_vk.pipelines.clear();
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
    if (count == 1 || count % 300 == 0) {
        LOGI("Render event %d #%d on thread %d (%s)", eventId, count, gettid(), rendererName(g_renderer));
    }
    if (eventId != kDrawEventId) return;
    if (g_renderer == kUnityGfxRendererOpenGLES30) {
        drawGlesDial();
    } else if (g_renderer == kUnityGfxRendererVulkan) {
        drawVulkanDial();
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

// Called from C# on Unity's main thread once per frame, before the render
// event; the render thread picks the values up when it draws.
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API HudGfx_SetDial(float value, int width, int height) {
    g_needleValue.store(value);
    g_targetWidth.store(width);
    g_targetHeight.store(height);
}
