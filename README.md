# Vehicle HUD

A small native Android app built as prep for a Unity "Senior SWE, Embedded
Android" interview (automotive Android/AOSP team) — a live vehicle-telemetry
HUD rendered directly via EGL/OpenGL ES on its own thread, fed by synchronized
C++ telemetry from two independent sources (an internal fake generator and an
AIDL service running in a separate process). Built in 5 days around a
full-time job. See the Claude Code plan file
`~/.claude/plans/will-this-cover-aosp-sleepy-pebble.md` for the original
week's design brief.

## Architecture

Four threads, one process boundary:

```
 UI thread (MainActivity)
   │ binds to, polls every ~100ms
   ▼
 :telemetry process (TelemetryService)
   │ AIDL / Binder IPC
   ▼
 TelemetryAidlPoll thread (MainActivity)
   │ JNI: pushExternalTelemetry() → setExternalTelemetry() → setExternal()
   ▼
 TelemetrySource (C++, mutex-guarded latest-value slot)
   ▲
   │ internal generator thread writes here too, until external data arrives
   │
 Render thread (RenderThread, owns the EGL context)
   reads TelemetrySource.latest() each frame → draws → eglSwapBuffers
```

- **Render thread** (`RenderThread`, `render_thread.cpp`): the only thread
  that ever touches EGL/GL. Created in `surfaceCreated`, torn down
  (`eglMakeCurrent(NO_CONTEXT)` → destroy context/surface → terminate) in
  `surfaceDestroyed`, always on the same thread it was created on.
- **Internal telemetry thread** (`TelemetrySource`, `telemetry_source.cpp`):
  generates fake speed/RPM at ~10Hz into a mutex-guarded slot. Stops writing
  the moment external data arrives (an atomic flag), so it hands off cleanly
  instead of racing with the AIDL-fed values.
- **AIDL poll thread** (`MainActivity.kt`): binds to `TelemetryService`,
  which runs in a **separate OS process** (`android:process=":telemetry"`),
  and polls it at ~10Hz on its own thread — deliberately never the UI thread
  or the render thread, since a synchronous Binder call blocks whatever
  thread calls it.
- **UI thread**: only owns the `ServiceConnection` callback and Activity
  lifecycle; never touches EGL, GL, or the telemetry mutex directly.

Two more systems exist alongside all of this, both reachable from the HUD:
**`VulkanActivity`/`VulkanRenderer`** (Phase 3, own render thread, own
teardown discipline, zero shared state with the diagram above) via tap,
kept deliberately separate so a Vulkan mistake could never touch the
working system described here; and **Unity's own
`UnityPlayerGameActivity`** (Phase 4, UAAL — a real `unityLibrary` Gradle
module merged into this same app, not this project's code) via long-press.

## What this demonstrates, and what it honestly doesn't (AOSP scope)

Built as an **app-layer NDK project** — C/C++, threading, EGL/OpenGL ES, and
AIDL as a client — deliberately scoped to fit a Mac dev environment with no
Linux box. It does **not** touch the real AOSP source tree, a HAL
implementation registered in the OS image, or the Soong/Blueprint build
system — that would require a Linux host (Cuttlefish needs KVM) and was
explicitly out of scope for a 5-day week around a full-time job. Said
directly, this is the honest scope: *"app-layer C++/threading/graphics/IPC,
not platform-layer AOSP — and here's specifically why, and what I'd need to
go further."*

## Talking points for the interview

Seven genuine, specific stories from this project, not generic claims:

1. **A real bug, found by reading a stack trace, not guessing.** The AIDL
   binder's first implementation — `override fun getSpeedKph(): Float =
   speedKph` — recursed into itself and StackOverflowed the `:telemetry`
   process on the very first call. Root cause: Kotlin synthesizes a property
   from any Java getter, so inside the anonymous object implementing the
   AIDL-generated `ITelemetryService` interface, the bare name `speedKph`
   resolved to the interface's own synthetic property on `this` — shadowing
   the outer class's same-named field — not the field I meant. Fixed by
   renaming the backing fields. Full detail in the Day 3 log entry below.
2. **A profiling result that wasn't the win I expected — and knowing what
   that means.** Measured a ~3.5ms/frame baseline, fixed a real
   redundant-GL-state-call pattern (four calls moved from every-frame to
   one-time init), re-measured, and found the improvement was within noise.
   Rather than stop there, split the timing further and found
   `eglSwapBuffers` — vsync/present-pipeline blocking — accounts for ~70% of
   the frame cost, which is expected/correct behavior, not a bug. The fix
   was still worth making (cleaner code, would matter more with a
   non-shadowing driver or more draw calls); the point is recognizing when
   an identified inefficiency isn't the actual bottleneck. Full detail in
   the Day 4 log entry below.
3. **The honest gap answer.** No production C/C++, Vulkan/OpenGL, or AOSP
   platform-layer experience — that's true and this project doesn't erase
   it. What it demonstrates instead: the same underlying bug classes
   (threading, lifecycle, ownership, IPC boundaries) that 8 years of
   production Android work already covers, applied one layer lower, on
   genuinely new ground, in five days.
4. **Verifying an assumption against the real toolchain, not the spec.**
   Attempted a C++20 `std::jthread` refactor; the NDK's clang accepted
   `-std=c++20` but its libc++ simply doesn't implement `std::jthread`/
   `std::stop_token` — compiler-standard support and standard-library
   support are different things. Reverted cleanly with the reason
   documented, rather than forcing a broken build or silently downgrading
   without saying why. Separately, capturing a real Perfetto trace of the
   render loop's `ATrace` markers required the `-a <package>` flag — the
   first capture attempt silently returned zero of our sections, and
   finding that out (not assuming success) is the same instinct as the
   AIDL bug in point 1: verify, don't assume. Full detail in the Phase 1
   log entry below.
5. **Read the real AOSP Vehicle HAL, and it's more relevant than expected.**
   Sparse-checked-out `automotive/vehicle` from the official AOSP mirror (no
   build) and compared it against `ITelemetryService` line by line — see
   `docs/AOSP_VHAL_NOTES.md`. The real VHAL is generic/property-based
   (`getValues`/`setValues`/`subscribe` keyed by `[propId, areaId]`, not
   per-signal methods), async with shared-memory fallback for scale, and
   VINTF-registered as a privileged `early_hal` service rather than
   `bindService()`-discovered. It also has a **real, current Rust
   implementation** (`aidl/rust_impl/vhal/`, built via Soong's
   `rust_binary`, using `libbinder_rs`) sitting right next to the C++ one —
   directly, concretely answers the JD's "exposure to Rust in AOSP" line
   with a specific file, not a hope.
6. **Vulkan, built and verified, not just read about.** A required JD line
   the original 5-day scope explicitly skipped. Built as an isolated
   second path (`VulkanActivity`, tap the HUD to reach it) so a setup
   mistake couldn't touch the working GLES app — full real pipeline
   (instance → device → swapchain → render pass → pipeline → command
   buffers → semaphore/fence sync), verified on-device with a real GPU
   detected (`Mali-G78`) and a triangle actually on screen, plus two clean
   teardown/re-init cycles. The real comparison: GLES hides synchronization
   behind an implicit state machine; Vulkan makes every sync point an
   object you create yourself — more code, but the explicit control the JD
   frames as the point of using it on resource-constrained hardware.
7. **Unity as a Library, merged into a real APK, with a real toolchain wall
   hit and cleared.** Exported a Unity 6 IL2CPP project as a Gradle library
   and merged it directly into this app — confirmed by inspecting the
   built APK's `lib/arm64-v8a/` and finding `libunity.so`/`libil2cpp.so`
   sitting next to this project's own native libraries, not just a
   passing build log. Doing that required a major AGP 8→9 / Gradle 8→9
   migration (Unity's hard requirement), taken seriously: checked AGP
   9.0.0 was a real published version before committing to it, `git
   init`'d the project and committed a known-good checkpoint *before*
   attempting the risky bump, then hit and fixed four distinct real
   breaking changes in order (AGP 9's built-in Kotlin conflicting with the
   old plugin, missing Unity-specific Gradle properties, a missing
   plugin-classpath declaration, a minSdk floor bump) — verified on-device
   afterward with real Unity log output (`Scripting Backend 'il2cpp'`,
   real device model, `Fully drawn`) and two screenshots proving the cube
   is actually rotating, not frozen.

## Day-by-day log

- **Day 1 (done, verified on-device):** `HudSurfaceView` (Kotlin) owns a
  native `RenderThread` (C++) for the life of its `Surface`. On
  `surfaceCreated`, `nativeSurfaceCreated()` wraps the `Surface` as an
  `ANativeWindow` and starts a dedicated thread that creates its own EGL
  context, clears and draws an animated triangle at ~60fps. On
  `surfaceDestroyed`, `nativeSurfaceDestroyed()` deletes the `RenderThread`,
  whose destructor stops the loop, joins the thread, and only then releases
  the `ANativeWindow` — all EGL/GL calls happen on that one thread and
  nowhere else.
  Verified with 4 repeated background/foreground cycles plus a rotation
  cycle on a physical device: EGL init/teardown log pairs matched 1:1 on a
  fresh thread ID each time, no leaks, no crash.
- **Day 2 (done, verified on-device):** `TelemetrySource` (C++) runs its own
  thread generating fake speed/RPM at ~10Hz, handed to the render thread
  through a short-held mutex — the writer locks only to assign two floats,
  `latest()` locks only to copy them out, so no lock is ever held across a
  GL call and the render loop never blocks waiting for new data. The
  triangle became a rotating needle whose angle is driven by that speed
  value. Verified via logcat across repeated background/foreground cycles:
  telemetry thread always starts before and stops after the render thread,
  no crashes; visually confirmed on-device as a smoothly sweeping needle.
- **Day 3 (done, verified on-device):** added `ITelemetryService` (AIDL) —
  `TelemetryService` runs in a genuinely separate process
  (`android:process=":telemetry"`), generating its own independent fake
  telemetry. `MainActivity` binds to it and polls it at ~10Hz from a
  dedicated thread (never the UI thread, never the render thread — a
  synchronous Binder call blocks whatever thread calls it), forwarding
  results into the native side via a small JNI bridge
  (`HudSurfaceView.pushExternalTelemetry` → `RenderThread::setExternalTelemetry`
  → `TelemetrySource::setExternal`). `TelemetrySource`'s internal generator
  checks an atomic flag each tick and stops writing once external data has
  arrived, so the two sources hand off cleanly instead of racing. Verified
  via `adb shell ps` that both processes exist
  (`dev.yosukesakurai.vehiclehud` and `dev.yosukesakurai.vehiclehud:telemetry`),
  and via logcat that the handoff happens exactly once, cleanly, with the
  needle visibly switching to the service's independent (different phase)
  telemetry.

  **Bug found and fixed along the way** — see Talking Points #1 above for
  the summary; full root cause: Kotlin synthesizes a property from any Java
  getter, and the AIDL-generated `ITelemetryService` interface has
  `getSpeedKph()` — so inside the anonymous object implementing that
  interface, the bare name `speedKph` resolved to *that* synthetic property
  on `this` (i.e. back to the getter being defined), shadowing the outer
  class's same-named field. Fixed by renaming the backing fields
  (`currentSpeedKph`/`currentRpm`) so there's no collision. Caught via `adb
  logcat` showing a `FATAL EXCEPTION` with `getSpeedKph()` repeated hundreds
  of times in its own stack trace.
- **Day 4 (done, verified on-device):** added always-on frame-time
  instrumentation around `renderFrame()` (rolling average logged every 300
  frames, ~5s at 60fps) and measured a real baseline before touching
  anything: **~3.4–3.8ms/frame**, average ~3.5ms, across three separate
  windows.

  **Fix applied:** `glClearColor`, `glUseProgram`, `glEnableVertexAttribArray`,
  and `glVertexAttribPointer` were being re-issued every frame even though
  none of them ever change — one program, one vertex layout, one clear
  color for the whole context lifetime. Moved all four into `initEgl()`
  (one-time), leaving only `glClear` + the per-frame `glUniform1f` angle
  upload + `glDrawArrays` + `eglSwapBuffers` in the hot loop.

  **Honest result:** re-measured after the fix — **~3.35–3.6ms/frame**,
  average ~3.44ms. That's within noise (~2%), not a clean win. Rather than
  claim a bigger improvement than actually happened, split the timing
  further into draw-call time vs. `eglSwapBuffers` time specifically, which
  showed the fix wasn't wrong, it just wasn't where the real cost was:

  ```
  draw avg: 997 us, swap avg: 2378 us   (over 300 frames)
  draw avg: 968 us, swap avg: 2551 us
  draw avg: 949 us, swap avg: 2427 us
  ```

  `eglSwapBuffers` accounts for **~70% of the per-frame cost** — it's
  blocking on the display's present/vsync pipeline, which is expected,
  correct behavior for a windowed EGL surface synced to the compositor,
  not a bug to chase away (removing that sync would just trade a
  consistent frame time for tearing). The redundant-state fix was still
  worth making — it's objectively cleaner code and would matter more on a
  driver that doesn't already shadow redundant state internally, or with
  more draw calls per frame — but the actual bottleneck on this device is
  the presentation pipeline, not CPU-side GL call overhead. That's the
  more useful profiling story: not every identified inefficiency is the
  real bottleneck, and knowing how to tell the difference (split the
  timing, don't just eyeball one number) is the actual skill.
- **Day 5 (done):** this write-up. Restructured the README to lead with
  Architecture, the AOSP-scope honesty statement, and three concrete
  talking points, keeping the day-by-day detail below as supporting
  material rather than the first thing a reader sees. No new code.

- **Phase 1 (done, verified on-device) — modern C++ attempt + real
  profiling tooling.** First expansion beyond the original 5-day scope, now
  treated as ongoing skill-building for the whole 4-round process rather
  than a Thursday-only deadline.

  **Attempted:** refactor `RenderThread`/`TelemetrySource` from manual
  `std::thread` + `running_` atomic + explicit `join()` to C++20
  `std::jthread` (auto-joining, cooperative cancellation via
  `std::stop_token`, no manual stop-flag needed).

  **Real finding, not a mistake in the code:** it doesn't compile. NDK
  28.2.13676358's clang accepts `-std=c++20` for language features, but its
  bundled libc++ has no `std::jthread`/`std::stop_token` at all —
  `error: no type named 'stop_token' in namespace 'std'`. Compiler-standard
  support and standard-library support are different things, and this is a
  real, verifiable example of that gap on an actual embedded/mobile
  toolchain, not a hypothetical. Reverted the threading to the working
  `std::thread` + atomic pattern; kept `-std=c++20` (`CMakeLists.txt`,
  `set(CMAKE_CXX_STANDARD 20)`) since other C++20 language features remain
  available, just not this specific library feature.

  **Real profiling tooling added successfully:** wrapped the draw calls and
  `eglSwapBuffers` in `<android/trace.h>` `ATrace_beginSection`/
  `ATrace_endSection` markers (`render_thread.cpp`), on top of the existing
  always-on rolling logcat average from Day 4. Captured an actual Perfetto
  trace on-device:
  ```
  adb shell perfetto -o /data/misc/perfetto-traces/trace.pb -t 6s \
      -a dev.yosukesakurai.vehiclehud sched gfx view
  ```
  First attempt (without `-a <package>`) captured zero of our app-level
  sections — app-level `ATrace` sections need the app explicitly named to
  Perfetto's simplified CLI, not just the `gfx`/`view` category shortcuts.
  With `-a` added, the pulled trace contained exactly the expected slices:
  290 `VehicleHud::renderFrame` and 290 `VehicleHud::eglSwapBuffers` events
  over the capture window, ready to open in `ui.perfetto.dev` — this is how
  a real Android performance investigation is actually done, a material
  step up from raw `printf`-style timing.

  **Talking-point value of this phase specifically:** the failed jthread
  attempt is arguably more useful to mention than a clean success would
  have been — it shows the instinct to verify assumptions against the
  actual target toolchain rather than assume standard-conformance, and to
  revert cleanly with a documented reason rather than force a broken build.

- **Phase 2 (done) — read the real AOSP Vehicle HAL source, no build.**
  Sparse-checked-out just `automotive/vehicle` (12MB, not a full AOSP sync)
  from the official mirror:
  ```
  git clone --filter=blob:none --sparse --depth 1 \
      https://android.googlesource.com/platform/hardware/interfaces \
      reference/aosp-hardware-interfaces
  git -C reference/aosp-hardware-interfaces sparse-checkout set automotive/vehicle
  ```
  (`reference/` is gitignored — read-only research material, not part of
  this project.) Read the real `IVehicle.aidl`, `VehiclePropValue.aidl`,
  the default C++ impl's service registration
  (`aidl/impl/current/vhal/vhal-default-service.rc`/`.xml`), and — an
  unexpected find — a full **Rust** implementation of the same interface
  at `aidl/rust_impl/vhal/`. Full comparison against this project's toy
  `ITelemetryService` written up in `docs/AOSP_VHAL_NOTES.md`, cited
  against real file paths, not generic claims. See Talking Point #5 above
  for the summary.

- **Phase 3 (done, verified on-device) — a minimal Vulkan triangle.** A
  required JD line item the original 5-day scope explicitly skipped
  (OpenGL ES only). Built as a completely separate path —
  `VulkanActivity`/`VulkanSurfaceView` (Kotlin) and `VulkanRenderer`
  (`vulkan_renderer.h/.cpp`, C++), reachable by tapping the existing GLES
  HUD — so a Vulkan setup mistake couldn't jeopardize the working Day 1-5
  app. Same thread-ownership discipline as `RenderThread`: one dedicated
  thread owns every Vulkan call from creation through teardown.

  **Full real pipeline, deliberately scoped down in one place:**
  `VkInstance` → `VkSurfaceKHR` (from the same `ANativeWindow` pattern as
  the EGL path) → physical device selection (checks
  `vkGetPhysicalDeviceSurfaceSupportKHR` per queue family, not assumed) →
  logical device + queue → `VkSwapchainKHR` (FIFO present mode — vsync'd,
  same present discipline as the EGL path, for a fair comparison) → image
  views → render pass with an explicit subpass dependency → graphics
  pipeline (SPIR-V shaders compiled via `glslc`, loaded at runtime from
  `assets/shaders/` through `AAssetManager`) → framebuffers → command
  buffers, recorded once since the scene is static → semaphore/fence
  present synchronization. The one deliberate scope cut: vertex positions
  are hardcoded in `shaders/triangle.vert` via `gl_VertexIndex` rather than
  fed through a `VkBuffer` — skips device-memory allocation and staging
  entirely while still exercising every other real stage.

  **Verified on-device, not just compiled:** real GPU detected and logged
  (`Selected physical device: Mali-G78`), swapchain created matching the
  actual screen resolution (`1080x2400, 4 images`), validation-layer check
  correctly found none installed on this device and skipped gracefully
  rather than assuming, and — the actual point — a real triangle rendered
  on screen. Cycled through two full back/re-launch navigations: clean
  `Vulkan renderer torn down` → fresh re-init → clean triangle again, twice
  in a row, zero crashes, zero Vulkan errors in logcat.

  **Vulkan vs. GLES, concretely, not abstractly:** the GLES path
  (`render_thread.cpp`) needed roughly 250 lines for the same "draw one
  shape, driven by live data" job. The Vulkan path needed roughly 500+
  lines for a *static* triangle with no live data at all — and that's with
  vertex-buffer setup skipped. The difference is exactly the JD's framing:
  GLES hides synchronization behind an implicit, context-bound state
  machine; Vulkan makes every synchronization point a real object you
  create and manage yourself (the render-pass subpass dependency, the
  image-available/render-finished semaphores, the in-flight fence) — more
  code, but predictable, explicit frame timing instead of an implicit
  "just works" that gives you less control on resource-constrained
  hardware. That tradeoff, not the triangle itself, is the actual talking
  point.

- **Phase 4 (done, verified on-device) — Unity as a Library (UAAL), for
  real.** The single most literally-named technology in the JD's first
  bullet, and the highest-risk phase. A separate `vehicle-hud-unity` Unity
  6000.5.9f1 project (rotating cube, IL2CPP scripting backend) was exported
  as an Android Gradle library (`unityLibrary`) and merged directly into
  this app — one real APK containing both `libvehiclehud.so` (this
  project's own EGL/Vulkan/AIDL native code) and Unity's own
  `libunity.so`/`libil2cpp.so`/`libgame.so`, confirmed by inspecting the
  built APK's `lib/arm64-v8a/` contents directly, not just a green build
  log. Reachable in-app via long-press on the HUD (tap still opens the
  Vulkan triangle from Phase 3); launches Unity's own
  `UnityPlayerGameActivity` by explicit component name, since that
  Activity is declared in `unityLibrary`'s manifest, not one this project
  owns.

  **The real wall this phase hit:** Unity 6000.5.9f1's export requires
  **AGP 9.0.0 / Gradle 9.1.0** — a major-version jump from this project's
  AGP 8.13.0 / Gradle 8.13, and only one AGP/Gradle version can apply to a
  single Gradle build. Verified AGP 9.0.0 was a real, published, stable
  version (not asking for something broken) before deciding whether to
  attempt the migration. Given the risk to the four already-verified
  phases, `git init`'d this project and committed the known-good state
  first as an explicit rollback point *before* attempting the bump — the
  kind of safety net a risky migration should have, not an afterthought.

  **What actually broke, in order, each with a real fix:**
  1. AGP 9's Kotlin support is now built in; applying the separate
     `org.jetbrains.kotlin.android` plugin alongside it is a hard error
     ("no longer required for Kotlin support since AGP 9.0"). Fixed by
     removing that plugin from `app/build.gradle.kts` and letting AGP 9
     handle Kotlin natively.
  2. `unityLibrary/build.gradle` reads several properties
     (`unityStreamingAssets`, `unity.androidSdkPath`,
     `unity.androidNdkPath`, `unity.androidNdkVersion`,
     `unity.installInBuildFolder`) that only existed in *Unity's own*
     exported `gradle.properties`, not this project's. Merged the ones
     `unityLibrary`'s build script actually references into this
     project's `gradle.properties` — deliberately **not** copying
     `android.builtInKotlin=false`, since Unity sets that because
     `unityLibrary` has zero Kotlin sources, but this app's own `:app`
     module now depends on AGP 9's built-in Kotlin support (see fix #1) —
     copying it blind would have broken the module that actually needs it.
  3. `com.android.library` had no plugin-classpath declaration anywhere in
     this project (it only ever declared `com.android.application`) —
     `unityLibrary` needs it. Added `android-library` to
     `gradle/libs.versions.toml` and applied it (`apply false`) at the
     root `build.gradle.kts`.
  4. Unity 6's minimum supported `minSdk` is 26; `unityLibrary`'s own
     `build.gradle` hard-sets `minSdk 26`. Bumped this app's `minSdk` from
     24 to 26 to match — flagged during Phase 4 planning (a Unity API
     obsolescence warning caught while writing the export script), fixed
     for real here.

  **Verified on-device, not just compiled:** installed, long-pressed the
  HUD, and logcat showed the real thing — `Scripting Backend 'il2cpp'`,
  `Device Model 'Google Pixel 6a'`, `Fully drawn ... +622ms`, zero crashes.
  Two screenshots two seconds apart differ, confirming the cube is
  genuinely rotating (`RotateCube.cs`'s `Update()` running), not a frozen
  first frame. One more honest finding, not fixed: pressing back doesn't
  exit Unity's activity back to the native HUD — `UnityPlayerGameActivity`
  intercepts the back gesture itself (likely for in-game menu handling),
  so returning to `MainActivity` currently needs a fresh launch rather than
  back-navigation. Noted, not papered over.

## Build

```
./gradlew :app:assembleDebug
```

Requires the Android SDK with NDK `28.2.13676358` and CMake `3.22.1`. Gradle
is pinned to Android Studio's bundled JBR (JDK 21) via
`gradle.properties` (`org.gradle.java.home`), so the CLI and the IDE always
build with the same JVM.
