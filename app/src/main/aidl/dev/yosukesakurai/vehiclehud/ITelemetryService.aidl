package dev.yosukesakurai.vehiclehud;

import dev.yosukesakurai.vehiclehud.ITelemetryListener;

// Started as two synchronous getters that MainActivity polled at 10 Hz from
// its own thread. Now the service pushes samples to registered listeners
// instead (the way the Vehicle HAL's subscribe() works), and the getters
// stay for one-off reads.
interface ITelemetryService {
    float getSpeedKph();
    float getRpm();

    // oneway: the caller (MainActivity's UI thread, when it starts or stops
    // the speed source) never blocks on the :telemetry process.
    oneway void registerListener(ITelemetryListener listener);
    oneway void unregisterListener(ITelemetryListener listener);
}
