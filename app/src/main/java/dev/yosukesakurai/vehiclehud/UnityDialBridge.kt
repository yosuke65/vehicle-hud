package dev.yosukesakurai.vehiclehud

import android.util.Log

/**
 * Feeds vehicle speed to the Unity scene's native rendering plugin
 * (libhudgfx.so, source in unity-plugin/), which draws the speed dial on
 * Unity's render thread. The library ships inside the Unity export and is
 * the same instance Unity loads, so a JNI call here lands in the atomic the
 * render thread reads. Called on the telemetry thread; the native side only
 * stores a float, so it doesn't block.
 */
object UnityDialBridge : TelemetryConsumer {
    private const val TAG = "UnityDialBridge"

    private val loaded: Boolean = try {
        System.loadLibrary("hudgfx")
        true
    } catch (e: UnsatisfiedLinkError) {
        Log.w(TAG, "libhudgfx not in this build; the Unity dial won't get speed", e)
        false
    }

    override fun onTelemetry(speedKph: Float, rpm: Float) {
        if (loaded) nativeSetSpeedKph(speedKph)
    }

    private external fun nativeSetSpeedKph(kph: Float)
}
