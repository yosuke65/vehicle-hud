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
 */
class VulkanSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private var rendererHandle: Long = 0L

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        rendererHandle = nativeSurfaceCreated(holder.surface, context.assets)
        if (rendererHandle == 0L) {
            Log.e(TAG, "Failed to start native Vulkan renderer")
        }
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
        if (rendererHandle != 0L) {
            nativeSurfaceDestroyed(rendererHandle)
            rendererHandle = 0L
        }
    }

    private external fun nativeSurfaceCreated(surface: Surface, assetManager: AssetManager): Long
    private external fun nativeSurfaceDestroyed(handle: Long)
    private external fun nativeSurfaceChanged(handle: Long)

    companion object {
        private const val TAG = "VulkanSurfaceView"

        init {
            System.loadLibrary("vehiclehud")
        }
    }
}
