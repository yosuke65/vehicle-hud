package dev.yosukesakurai.vehiclehud

import java.util.concurrent.CopyOnWriteArrayList

/**
 * Thread-safe fan-out telemetry bus. The speed source pushes samples here;
 * every registered consumer (primary HUD, secondary HUD Presentation, the
 * Vulkan screen, the Unity plugin bridge) receives them on the pushing
 * thread. Consumers are expected to be non-blocking, which they all are.
 */
class TelemetryRouter {
    private val consumers = CopyOnWriteArrayList<TelemetryConsumer>()

    // addIfAbsent: an activity that's created twice must not get every
    // sample twice.
    fun add(consumer: TelemetryConsumer) = consumers.addIfAbsent(consumer)
    fun remove(consumer: TelemetryConsumer) = consumers.remove(consumer)

    fun push(speedKph: Float, rpm: Float) {
        consumers.forEach { it.onTelemetry(speedKph, rpm) }
    }

    companion object {
        /**
         * One router per process. MainActivity owns the speed source, but
         * other screens (the Vulkan activity, Unity) are separate activities
         * that still need the same stream.
         */
        val shared = TelemetryRouter()
    }
}
