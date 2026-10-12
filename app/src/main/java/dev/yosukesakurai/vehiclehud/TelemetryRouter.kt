package dev.yosukesakurai.vehiclehud

import java.util.concurrent.CopyOnWriteArrayList

/**
 * Thread-safe fan-out telemetry bus. A single polling thread pushes samples
 * here; every registered consumer (primary HUD, secondary HUD Presentation,
 * future displays) receives them on that same thread. Consumers are expected
 * to be non-blocking, which HudSurfaceView already is.
 */
class TelemetryRouter {
    private val consumers = CopyOnWriteArrayList<TelemetryConsumer>()

    fun add(consumer: TelemetryConsumer) = consumers.add(consumer)
    fun remove(consumer: TelemetryConsumer) = consumers.remove(consumer)

    fun push(speedKph: Float, rpm: Float) {
        consumers.forEach { it.onTelemetry(speedKph, rpm) }
    }
}
