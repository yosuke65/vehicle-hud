package dev.yosukesakurai.vehiclehud;

// Pushed by TelemetryService at about 10 Hz. oneway: the service never waits
// for a client, so a slow or stuck client can't stall the generator thread.
oneway interface ITelemetryListener {
    void onTelemetry(float speedKph, float rpm);
}
