package dev.yosukesakurai.vehiclehud

import android.app.Presentation
import android.content.Context
import android.view.Display

/**
 * Default secondary-display policy: show a mirrored GLES HUD.
 */
class HudPresentationPolicy(
    private val context: Context,
    private val telemetryRouter: TelemetryRouter,
) : DisplayPolicy {
    override fun createPresentation(display: Display): Presentation =
        HudPresentation(context, display, telemetryRouter)
}
