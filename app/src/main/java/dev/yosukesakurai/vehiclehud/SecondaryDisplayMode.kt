package dev.yosukesakurai.vehiclehud

/**
 * Runtime mode for secondary display content. Toggled via ADB broadcast so
 * the demo can switch between GLES/EGL and Vulkan windowing without adding
 * permanent UI to the HUD SurfaceView.
 */
enum class SecondaryDisplayMode {
    HUD,
    VULKAN,
}
