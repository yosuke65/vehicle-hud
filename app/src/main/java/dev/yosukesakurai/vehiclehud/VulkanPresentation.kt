package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.content.Context
import android.os.Bundle
import android.view.Display

/**
 * Presentation that runs the isolated Vulkan triangle renderer on a secondary
 * display. The VulkanSurfaceView creates its own native VulkanRenderer and
 * VkSurfaceKHR bound to this Presentation's window, so this exercises Vulkan's
 * Android WSI path (VK_KHR_android_surface + swapchain) on a secondary display.
 */
class VulkanPresentation(
    context: Context,
    display: Display,
) : Presentation(context, display) {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(VulkanSurfaceView(context))
    }
}
