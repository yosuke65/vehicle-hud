# Notes: our toy AIDL service vs. the real AOSP Vehicle HAL

Phase 2 of the interview-prep expansion (see `~/.claude/plans/will-this-cover-aosp-sleepy-pebble.md`):
read the actual AOSP Vehicle HAL (VHAL) source, no build, and compare it
honestly against `ITelemetryService`/`TelemetryService` in this project.

**Source:** sparse-checkout of `automotive/vehicle` from the official AOSP
mirror, no full sync, no build:

```
git clone --filter=blob:none --sparse --depth 1 \
    https://android.googlesource.com/platform/hardware/interfaces \
    reference/aosp-hardware-interfaces
cd reference/aosp-hardware-interfaces
git sparse-checkout set automotive/vehicle
```

(`reference/` is gitignored — it's read-only research material, not part of
this project.) File paths below are relative to
`reference/aosp-hardware-interfaces/automotive/vehicle/`.

## 1. Generic property API vs. our per-signal methods

Our `ITelemetryService.aidl`:

```aidl
interface ITelemetryService {
    float getSpeedKph();
    float getRpm();
}
```

One method per signal. Adding a third signal means adding a third method
and rebuilding every client.

The real interface, `aidl/android/hardware/automotive/vehicle/IVehicle.aidl`,
has no per-signal methods at all. Instead it's **generic and
property-based**:

```aidl
void getValues(IVehicleCallback callback, in GetValueRequests requests);
void setValues(IVehicleCallback callback, in SetValueRequests requests);
void subscribe(in IVehicleCallback callback, in SubscribeOptions[] options,
        int maxSharedMemoryFileCount);
```

Every signal — speed, RPM, door lock state, HVAC fan speed, hundreds of
others — is a `[propId, areaId]` pair passed into the *same* three methods.
Adding a new vehicle signal doesn't touch the interface at all; it's a new
enum entry, not a new method. `VehiclePropValue.aidl` is the generic
envelope every value travels in — property ID, area ID, a status, a
timestamp, and a `RawPropValues` union for the actual data.

**Why ours is different, honestly:** a real VHAL has to support an entire
vehicle's signal catalog without changing its interface every time a new
signal is added. A toy service exposing two hardcoded signals has no such
requirement — the generic design would have been pure overhead for what
this project needed. But it's the first thing that would need to change if
this were ever meant to scale past a demo.

## 2. Async callback + shared memory vs. our synchronous getters

Ours: `getSpeedKph()` returns a `float` directly — a plain synchronous
Binder call, blocking the caller for the round-trip (which is exactly why
`MainActivity`'s poll happens on its own thread, never the UI thread).

The real one: `getValues()` takes an `IVehicleCallback` and returns nothing
directly — results arrive later via `IVehicleCallback.onGetValues()`,
possibly split across multiple calls, and for large result sets, via a
**shared memory file** instead of the direct Binder reply at all (the doc
comment explicitly calls out "binder memory limitation"). This is designed
for potentially hundreds of properties queried at once across many
processes — a scale problem our two-getter toy never has to solve.

## 3. `@VintfStability` and how the HAL actually gets registered

`IVehicle.aidl` is annotated `@VintfStability` — a real system/vendor
interface-boundary marker that locks the interface's ABI so framework and
vendor code (potentially built by different parties, at different times)
stay compatible. App-layer AIDL like ours never needs this; it's compiled
and used entirely within one app's own build.

The real service registers itself through two files our app-layer service
has no equivalent of, found at
`aidl/impl/current/vhal/vhal-default-service.rc` and `.xml`:

```
# vhal-default-service.rc
service vendor.vehicle-hal-default /vendor/bin/hw/android.hardware.automotive.vehicle@V4-default-service
    class early_hal
    user vehicle_network
    group system inet
```

```xml
<!-- vhal-default-service.xml — a VINTF manifest fragment -->
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.automotive.vehicle</name>
        <version>4</version>
        <fqname>IVehicle/default</fqname>
    </hal>
</manifest>
```

`class early_hal` means this starts *before* most of the system, as a
privileged `vendor_network`-group process — nothing like our regular,
unprivileged `:telemetry` app process, which only exists once
`bindService()` is called from an already-running app. Our service is
discovered by Intent + package name; the real one is discovered by the
framework through this VINTF manifest, matched against what the framework
actually expects to find at boot.

## 4. Rust — a real, current example, not a hypothetical

The JD lists "exposure to Rust in AOSP" as a nice-to-have. This checkout
has a genuine, current one: `aidl/rust_impl/vhal/`, a full Rust
implementation of the exact same `IVehicle` interface, built via Soong's
`rust_binary` module type:

```
# aidl/rust_impl/vhal/Android.bp
rust_binary {
    name: "android.hardware.automotive.vehicle-V3-rust-service",
    vendor: true,
    srcs: ["src/*.rs"],
    crate_root: "src/main.rs",
    rustlibs: ["libbinder_rs"],
    vintf_fragments: ["vhal-rust-service.xml"],
    init_rc: ["vhal-rust-service.rc"],
}
```

```rust
// aidl/rust_impl/vhal/src/main.rs
use android_hardware_automotive_vehicle::aidl::android::hardware::automotive::vehicle::IVehicle::BnVehicle;
use crate::default_vehicle_hal::DefaultVehicleHal;

fn main() {
    binder::ProcessState::start_thread_pool();
    let my_service = DefaultVehicleHal;
    let my_service_binder = BnVehicle::new_binder(my_service, binder::BinderFeatures::default());
    binder::add_service("android.hardware.automotive.vehicle.IVehicle/default", my_service_binder.as_binder())
        .expect("Failed to register");
    binder::ProcessState::join_thread_pool()
}
```

Same AIDL interface (`IVehicle`), same Binder registration pattern as the
C++ default impl — just via `libbinder_rs` instead of libbinder's C++ API.
Also worth noting: `VehiclePropValue.aidl` itself carries a
`@RustDerive(Clone=true)` annotation — the AIDL compiler generates real Rust
bindings for this interface as a matter of course, not as a special case.

## Honest summary

This project's `ITelemetryService` is a legitimate, correctly-built
app-layer AIDL service — the IPC boundary, the separate-process split, the
never-block-the-UI/render-thread discipline are all real and correctly
applied. What it is *not*, and what reading this source makes concrete
rather than abstract: a platform-layer HAL. The real thing is generic
instead of per-signal, async/shared-memory instead of synchronous, VINTF-
registered instead of `bindService()`-discovered, stability-locked instead
of app-private, and — as of this AOSP revision — has a real, current Rust
implementation sitting right next to the C++ one. That's the concrete
version of "I know what I don't have production experience with yet."
