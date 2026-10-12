package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.view.Display

/**
 * Pluggable policy for what content to show on a discovered secondary display.
 * Lets DisplayTracker switch between, e.g., a mirrored GLES HUD and a Vulkan
 * renderer without changing its display-lifecycle logic.
 */
fun interface DisplayPolicy {
    fun createPresentation(display: Display): Presentation
}
