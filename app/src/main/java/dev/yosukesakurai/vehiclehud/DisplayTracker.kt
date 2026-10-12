package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.content.Context
import android.hardware.display.DisplayManager
import android.util.Log
import android.view.Display

/**
 * Listens to DisplayManager events, creates a Presentation on every secondary
 * display, and tears it down when the display goes away. Supports switching the
 * content policy at runtime (e.g., GLES HUD -> Vulkan renderer).
 */
class DisplayTracker(
    private val context: Context,
    private var policy: DisplayPolicy,
) {
    private val displayManager = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val presentations = mutableMapOf<Int, Presentation>()

    private val listener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = handleDisplayAdded(displayId)
        override fun onDisplayChanged(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) = dismiss(displayId)
    }

    fun start() {
        displayManager.registerDisplayListener(listener, null)
        displayManager.displays
            .filter { it.displayId != Display.DEFAULT_DISPLAY }
            .forEach { handleDisplayAdded(it.displayId) }
    }

    fun stop() {
        displayManager.unregisterDisplayListener(listener)
        presentations.keys.toList().forEach(::dismiss)
    }

    /**
     * Switches the content policy for all currently-connected secondary
     * displays and any future ones. Dismisses and re-creates existing
     * presentations so the change is visible immediately.
     */
    fun setPolicy(newPolicy: DisplayPolicy) {
        if (policy::class == newPolicy::class) return
        policy = newPolicy
        val ids = presentations.keys.toList()
        ids.forEach { displayId ->
            dismiss(displayId)
            handleDisplayAdded(displayId)
        }
    }

    fun hasSecondaryDisplay(): Boolean = presentations.isNotEmpty()

    fun firstDisplayId(): Int? = presentations.keys.firstOrNull()

    private fun handleDisplayAdded(displayId: Int) {
        if (presentations.containsKey(displayId)) return
        val display = displayManager.getDisplay(displayId) ?: return
        Log.i(
            TAG,
            "Secondary display added: id=$displayId, " +
                "size=${display.mode.physicalWidth}x${display.mode.physicalHeight}"
        )
        val presentation = policy.createPresentation(display)
        presentation.show()
        presentations[displayId] = presentation
    }

    private fun dismiss(displayId: Int) {
        presentations.remove(displayId)?.dismiss()
        Log.i(TAG, "Secondary display removed: id=$displayId")
    }

    companion object {
        private const val TAG = "DisplayTracker"
    }
}
