package dev.yosukesakurai.vehiclehud

/**
 * Any surface/Activity that wants to receive vehicle telemetry samples
 * implements this. Used by TelemetryRouter to fan-out one telemetry stream
 * to multiple displays (primary HUD, secondary HUD, etc.).
 */
interface TelemetryConsumer {
    fun onTelemetry(speedKph: Float, rpm: Float)
}
