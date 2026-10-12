package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.content.Context
import android.os.Bundle
import android.view.Display

/**
 * Presentation that mirrors the primary GLES HUD on a secondary display.
 * Each HudSurfaceView creates its own native RenderThread with its own EGL
 * surface/context, so this automatically exercises EGL multi-surface windowing.
 */
class HudPresentation(
    context: Context,
    display: Display,
    private val telemetryRouter: TelemetryRouter,
) : Presentation(context, display) {

    private lateinit var hudView: HudSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hudView = HudSurfaceView(context)
        setContentView(hudView)
    }

    override fun onStart() {
        super.onStart()
        telemetryRouter.add(hudView)
    }

    override fun onStop() {
        telemetryRouter.remove(hudView)
        super.onStop()
    }
}
