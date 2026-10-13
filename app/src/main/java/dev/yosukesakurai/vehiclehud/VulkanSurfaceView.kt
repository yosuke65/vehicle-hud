package dev.yosukesakurai.vehiclehud

import android.content.Context
import android.content.res.AssetManager
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Same lifecycle ownership pattern as HudSurfaceView, for a completely
 * separate native VulkanRenderer instance — deliberately isolated from the
 * EGL/GLES path so a Vulkan setup mistake can't affect it.
 *
 * Draws the live speed dial: while it has a surface it's registered with the
 * shared TelemetryRouter, and each sample goes to the native side, which the
 * render thread reads when it records the next frame.
 */
class VulkanSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback, TelemetryConsumer {

    private var rendererHandle: Long = 0L

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        rendererHandle = nativeSurfaceCreated(holder.surface, context.assets)
        if (rendererHandle == 0L) {
            Log.e(TAG, "Failed to start native Vulkan renderer")
            return
        }
        TelemetryRouter.shared.add(this)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // The render thread checks the surface and rebuilds the swapchain
        // if the size or rotation really changed.
        Log.i(TAG, "surfaceChanged: ${width}x$height")
        if (rendererHandle != 0L) {
            nativeSurfaceChanged(rendererHandle)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        TelemetryRouter.shared.remove(this)
        if (rendererHandle != 0L) {
            nativeSurfaceDestroyed(rendererHandle)
            rendererHandle = 0L
        }
    }

    // On the telemetry thread. The native side only stores the value, so a
    // sample that arrives while the surface is being torn down is harmless.
    override fun onTelemetry(speedKph: Float, rpm: Float) = nativeSetSpeedKph(speedKph)

    private external fun nativeSurfaceCreated(surface: Surface, assetManager: AssetManager): Long
    private external fun nativeSurfaceDestroyed(handle: Long)
    private external fun nativeSurfaceChanged(handle: Long)

    companion object {
        private const val TAG = "VulkanSurfaceView"

        @JvmStatic
        private external fun nativeSetSpeedKph(speedKph: Float)

        init {
            System.loadLibrary("vehiclehud")
        }
    }
}
