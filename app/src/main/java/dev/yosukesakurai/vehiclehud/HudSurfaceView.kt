package dev.yosukesakurai.vehiclehud

import android.content.Context
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Owns the lifecycle of the native render thread. One native RenderThread
 * instance is created per surfaceCreated() and torn down (thread joined,
 * EGL destroyed) per surfaceDestroyed() — the handle is only ever valid
 * between those two calls.
 */
class HudSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private var renderThreadHandle: Long = 0L

    init {
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        renderThreadHandle = nativeSurfaceCreated(holder.surface)
        if (renderThreadHandle == 0L) {
            Log.e(TAG, "Failed to start native render thread")
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // Geometry is read directly from the ANativeWindow on the native
        // side; nothing to do here for this minimal HUD.
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (renderThreadHandle != 0L) {
            nativeSurfaceDestroyed(renderThreadHandle)
            renderThreadHandle = 0L
        }
    }

    /**
     * Forwards a telemetry sample from an external source (currently the
     * AIDL TelemetryService, polled by MainActivity on its own thread) to
     * the native render thread's TelemetrySource, which takes over from
     * its own internal fake generator once this is called at least once.
     * A no-op when there's no live surface/render thread to receive it.
     */
    fun pushExternalTelemetry(speedKph: Float, rpm: Float) {
        val handle = renderThreadHandle
        if (handle != 0L) {
            nativeSetTelemetry(handle, speedKph, rpm)
        }
    }

    private external fun nativeSurfaceCreated(surface: Surface): Long
    private external fun nativeSurfaceDestroyed(handle: Long)
    private external fun nativeSetTelemetry(handle: Long, speedKph: Float, rpm: Float)

    companion object {
        private const val TAG = "HudSurfaceView"

        init {
            System.loadLibrary("vehiclehud")
        }
    }
}
