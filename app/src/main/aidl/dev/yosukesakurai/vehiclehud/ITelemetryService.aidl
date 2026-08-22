package dev.yosukesakurai.vehiclehud;

// Deliberately two plain synchronous getters rather than a callback/AIDL
// oneway push API: this is meant to be polled from a background thread on
// the client side (see MainActivity), matching how a real HAL/service
// query would typically be shaped.
interface ITelemetryService {
    float getSpeedKph();
    float getRpm();
}
