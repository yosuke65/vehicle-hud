package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.content.Context
import android.view.Display

/**
 * Alternative secondary-display policy: show the Vulkan triangle renderer.
 */
class VulkanPresentationPolicy(
    private val context: Context,
) : DisplayPolicy {
    override fun createPresentation(display: Display): Presentation =
        VulkanPresentation(context, display)
}
