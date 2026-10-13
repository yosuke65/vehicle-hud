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

Started as an **app-layer NDK project** — C/C++, threading, EGL/OpenGL ES,
and AIDL as a client — deliberately scoped to fit a Mac dev environment with
no Linux box, explicitly deferring the real AOSP source tree, a HAL
implementation registered in the OS image, and the Soong/Blueprint build
system as out of scope for a 5-day week around a full-time job.

That deferred scope was later pursued for real, on a second physical
machine (an Intel MacBook wiped to Ubuntu). Real, verified outcomes: a
correct `repo`-synced AOSP checkout (1084/1084 projects) on the
Google-recommended `android-latest-release` branch after finding and fixing
a wrong-branch/wrong-config mistake against Google's own docs; genuine
Soong/Blueprint debugging (an `Android.bp` property version mismatch,
tracked to the exact line in Soong's source); two full Android Automotive
system builds (`sdv_core_cf`, `sdv_ivi_cf`) that actually compiled and
booted on Cuttlefish, the second with a real, screenshotted two-display
digital cockpit UI; and this same app installed and tested directly on that
self-built automotive OS (Phase 7 below); and, going one step further, a
real code change to SurfaceFlinger itself and to a real Vehicle HAL
implementation (`FakeVehicleHardware::setValue`), each built through the
fast incremental platform-dev inner loop and verified live on the running
device via `adb logcat` — genuine implementation-level touches, not just
build-only exposure. What's still genuinely not touched: HWComposer/Gralloc
specifically, and no HAL authored fully from scratch (the edit was to an
existing fake implementation, not a new HAL). Said directly, the honest
scope is now: *"app-layer C++/threading/graphics/IPC, plus real hands-on
AOSP platform/build-system work, a real automotive boot, and verified
edits to the compositor and a Vehicle HAL — and here's specifically what's
still untouched."*

## Talking points for the interview

Nine genuine, specific stories from this project, not generic claims:

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
8. **Knowing when to stop, and why that's the more honest answer.**
   Attempted genuine same-screen Unity+native compositing (Unity as the
   background, the native needle as an inset overlay, one shared telemetry
   value driving both). Verified the API supported it via `javap` on the
   real compiled classes, not assumption; hit and fixed two real Gradle
   visibility/path issues; then hit a genuine architectural wall —
   `UnityPlayerForGameActivity`'s native startup silently depends on
   `GameActivity`'s own native event glue, which never runs unless the
   host Activity actually extends `GameActivity`. Diagnosed methodically
   (`dumpsys` view hierarchy, timed waits to rule out a slow cold start,
   confirmed the main thread wasn't blocked) rather than guessed, then
   reverted cleanly back to the working Phase 4 state the moment the
   evidence was clear, instead of continuing to poke at decreasing odds.
   Full root-cause writeup in the Phase 5 log entry below.
9. **Real concurrency tests, not just working code.** The JD's one clean
   gap — "automated testing" — closed cheaply and honestly: three tests
   against the *actual* `TelemetrySource` production source (compiled
   directly, not reimplemented), covering the property that actually
   matters — that the internal/external telemetry handoff never produces
   a torn read under real concurrent pressure (4 writers + 1 reader, a
   deterministic invariant that a broken mutex would violate). Clean under
   ThreadSanitizer, not just passing by luck.
10. **When a guess turns out wrong, check the source of truth instead of
    guessing again.** Attempting a real AOSP Automotive build, the first
    `repo init` branch and lunch config were both guessed — one from a
    NOTICE banner half-read, one from an unrelated source comment — and
    hit a wall three errors deep (a foundational AIDL interface,
    `google.sdv.identity`, absent from every public AOSP repo). Instead of
    a fourth guess, read Google's actual `source.android.com` docs
    directly: both guesses were wrong, in specific, findable ways. Fixed
    both, re-synced, and every dependency error vanished — direct
    confirmation the root-cause read was correct, not just a fix that
    happened to work. Full detail in the Phase 7 log entry below.
11. **Cross-deploying this exact app onto a self-built automotive OS: three
    confirmed, one wall precisely diagnosed.** Installed this project's own
    debug APK onto a real AAOS system built and booted from AOSP source
    (Phase 7). AIDL/OpenGL ES confirmed via the app's own perf log firing
    on real automotive hardware, not just "it launched." Vulkan confirmed
    via `dumpsys window` genuinely switching focus, not a visual guess
    (its screenshot looked similar to the GLES view, which wasn't
    diagnostic on its own — the two shapes are both simple green geometry
    on black). Unity/UAAL crashed with `UnsatisfiedLinkError` for
    `libgame.so`; attempted the real fix (adding x86_64 as a target
    architecture) and got a definitive answer from Unity's own compiler
    warning that x86_64 Android support has been fully removed from this
    Editor version — a genuine product limitation, confirmed by trying the
    fix, not assumed from the crash alone.
12. **Knowing exactly which wall is real vs. an artifact of the test rig.**
    Checked the Unity/UAAL crash above against Unity's actual HMI product
    page (the one this role's automotive customers — Toyota, Mercedes-Benz,
    Mazda — are named on). Their pipeline is Unity Industry + Embedded
    platform support, a licensed enterprise product targeting ARM
    automotive SoCs — genuinely out of reach here, and I said so plainly
    rather than implying it was one tweak away. But that also means the
    x86_64 crash on Cuttlefish isn't evidence Unity can't do this — it's
    an artifact of Cuttlefish being an x86_64 virtual device, while this
    project's own Unity build is already ARM64-only and was separately
    verified working on real ARM64 Android hardware in Phase 4. Not
    confusing a licensing boundary with a technical one, or a test-rig
    limitation with a product limitation, is itself the platform-literacy
    the JD is asking for.

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

- **Phase 5 (attempted, real architectural wall found, reverted honestly)
  — genuine same-screen Unity + native compositing.** Went further than
  Phase 4's separate-Activity launch: tried constructing Unity's player
  directly inside `MainActivity` itself, so the Unity 3D scene would
  render as a full-screen background with the native GLES needle
  composited on top as a smaller inset, both driven by the same live
  telemetry (`UnitySendMessage` feeding the cube's rotation speed from the
  same AIDL poll loop that drives the needle).

  **Feasibility genuinely verified first, not assumed:** decompiled the
  real `unity-classes.jar` (`javap -classpath . com.unity3d.player.X`) and
  confirmed `UnityPlayerForGameActivity`'s constructor takes a plain
  `android.app.Activity`, not specifically a `GameActivity` — same-Activity
  embedding looked genuinely possible at the API level, not a guess.

  **Built it, hit two real, fixable Gradle issues first:**
  1. `unity-classes.jar` is only an `implementation` dependency *inside*
     `unityLibrary`, invisible to `:app`'s compile classpath even though
     it's in the final APK at runtime — fixed with a direct `compileOnly`
     dependency in `app/build.gradle.kts` pointing at the same jar.
  2. A relative-path depth bug in that fix (`app/build.gradle.kts`
     resolves relative paths from `vehicle-hud/app/`, not `vehicle-hud/`,
     unlike `settings.gradle.kts`) — needed one more `../`.

  **Then hit a real architectural wall, not a wiring bug.** After fixing
  both Gradle issues, the app built and ran with zero crashes — but
  Unity's background never rendered. Diagnosed methodically, not guessed:
  `adb shell dumpsys activity` showed the container `FrameLayout` had zero
  children (the constructor doesn't auto-attach the `SurfaceView` the way
  `GameActivity`'s own framework does — fixed by attaching it manually);
  after that fix, the view hierarchy was correct, but Unity's log still
  stopped dead after exactly one line (`IL2CPP: JNI_OnLoad` →
  `Unity: Context Type: GameActivity` → nothing), with the Activity itself
  healthy (`mResumed=true`, no ANR — the main thread wasn't blocked, so
  something in Unity's own internal thread was). Waited 15+ extra seconds
  to rule out a slow cold start — no change. Conclusion: `UnityPlayerFor
  GameActivity`'s constructor is Activity-agnostic *at the API surface*
  (confirmed by `javap`), but its native startup isn't actually
  host-agnostic at runtime — it appears to depend on `GameActivity`'s own
  native event glue (the C++ side of `com.google.androidgamesdk
  .GameActivity`, which normally pumps `APP_CMD_INIT_WINDOW`-style events
  into Unity's engine), which never initializes unless the host Activity
  genuinely *extends* `GameActivity`. `MainActivity` extends plain
  `ComponentActivity`, so that glue never runs, and Unity's render thread
  waits on a signal that never arrives.

  **Reverted cleanly, not left half-broken:** `git checkout` back to the
  Phase 4 commit for `MainActivity.kt`/`app/build.gradle.kts`, deleted the
  new `EmbeddedUnityView.kt` — the app is back to Phase 4's verified,
  working separate-Activity Unity launch. This finding is documented here
  instead of hidden specifically *because* it's good material: verifying
  an API surface with real tooling (`javap`, not assumption) instead of
  trusting documentation, methodically diagnosing a silent runtime stall
  with the actual tools (`dumpsys`, live view hierarchy, timed waits to
  rule out "just slow") rather than guessing, and reverting cleanly the
  moment the evidence pointed at a real architectural constraint rather
  than continuing to poke at decreasing odds — that whole process is a
  more honest answer to "tell me about Unity as a Library" than a cube
  that happens to render would have been on its own.

- **Phase 6 (done) — targeted concurrency tests for `TelemetrySource`.**
  The JD explicitly lists "automated testing," and until this phase the
  project had zero — a real, cheap gap to close, unlike the hardware ones.
  Rather than blanket coverage everywhere, tested the one place
  concurrency correctness actually matters: `TelemetrySource`'s
  mutex-guarded internal/external handoff. `native_tests/telemetry_source_test.cpp`
  compiles the **actual production** `telemetry_source.cpp` directly (not
  a reimplementation) as a standalone host binary — no test-framework
  dependency; a small custom runner is the honestly appropriate amount of
  infrastructure for three focused tests, not GoogleTest for two files.

  Three tests, each checking a real correctness property, not just "didn't
  crash":
  1. `setExternal()`'s value is immediately visible via `latest()`.
  2. The internal fake generator genuinely **stops** overwriting once
     `setExternal()` is called — sampled repeatedly across ~3.75x the
     generator's own tick interval, not just checked once (a single check
     could pass by coincidence if the generator hadn't ticked again yet).
  3. **No torn reads** under real concurrent pressure: 4 writer threads
     hammer `setExternal()` with values satisfying a deterministic
     invariant (`rpm == speedKph * 10`, always) while a reader thread
     samples `latest()` concurrently for 300ms — if the mutex were broken
     or removed, a reader could observe `speedKph` from one write
     interleaved with `rpm` from a different write, breaking that
     invariant. This is the test that actually exercises the concurrency
     claim, not just the API.

  All three pass plain, **and clean under ThreadSanitizer**
  (`-fsanitize=thread`) — no data race reported, real verification beyond
  "the invariant happened to hold this run":
  ```
  clang++ -std=c++20 -pthread -fsanitize=thread -Wall -Wextra \
      native_tests/telemetry_source_test.cpp app/src/main/cpp/telemetry_source.cpp \
      -I app/src/main/cpp -o /tmp/telemetry_source_test_tsan && /tmp/telemetry_source_test_tsan
  ```

- **Phase 7 (done) — a real AOSP Automotive build, booted, with this app
  running on it.** The scope this project originally deferred (see above):
  pursued for real on a second physical machine, an Intel MacBook wiped to
  T2-patched Ubuntu 24.04, reachable over Tailscale.

  **Wrong branch, wrong release config — found by reading the actual
  official docs, not by guessing again.** First attempt: `repo init -b
  main` (AOSP trunk) plus a guessed `sdv_core_tiny_cf-trunk_staging-userdebug`
  lunch line, guessed from an unrelated source comment. Hit three real
  build errors in sequence — a Soong `Android.bp` property
  (`use_expansive_default_srcs`) that this `main`-branch Soong didn't
  recognize (verified all 140 occurrences were harmlessly `false` and
  stripped it), a dangling test fixture depending on already-removed
  sample code, and finally `google.sdv.identity` — a foundational AIDL
  interface imported by 21+ files, confirmed absent from every public repo
  on `android.googlesource.com`. That last one wasn't stub-able honestly,
  so instead of guessing a fourth fix, read Google's actual
  `source.android.com/docs/automotive/sdv` documentation directly. It
  states plainly, on multiple pages, to use the `android-latest-release`
  manifest branch and the `aosp_current` release config — both different
  from what had been guessed, and Google's own sync tooling had been
  printing a NOTICE recommending exactly this the whole time. Re-synced on
  the correct branch (1084/1084 projects; the SDV service repos turned out
  to already be wired into that branch's default manifest, no local
  manifest hack needed there), and every dependency error vanished — the
  first fix attempt was real config, the second attempt made it to 100% of
  Soong's own analysis with zero dependency errors, confirming the
  root-cause diagnosis was exactly right.

  **A real Soong bootstrap OOM, fixed with evidence, not guesswork.** That
  same 100%-complete analysis run got killed by the OOM killer
  (`exit=137`) in its very last step — 15GB RAM + 15GB swap wasn't enough
  to hold the full 1084-project dependency graph in memory for a single
  Go process. Added a 30GB swapfile (45GB total; the connected external
  drive had disk to spare) and reran the identical build:
  `lunch sdv_core_tiny_cf-aosp_current-userdebug && m -j4` succeeded in
  full — `Build Succeeded: 78216 steps` in ~8h22m — with real
  `boot.img`/`system.img`/`super.img` output matching exactly what the
  target's own product config says it should contain.

  **A visual automotive cockpit, not just a build log.** `sdv_core_cf` is
  headless (Google's own docs say so); building the visual `sdv_ivi_cf`
  variant instead (`Build Succeeded: 145857 steps`, ~15h57m — the fuller
  UI stack: SurfaceFlinger, SystemUI, WindowManager) and getting it to
  actually *boot* took real, sequential troubleshooting: no `cvd` wrapper
  installed (used this build's own `launch_cvd` directly); both `crosvm`
  and `qemu_cli` VM backends failing with an identical generic error,
  traced by reading Cuttlefish's own `vm_manager.cpp`/`config_utils.cpp`
  source to one shared gating check —`HostSupportsQemuCli()` — verifying a
  `cuttlefish-common` Debian package that wasn't installed and isn't in
  Ubuntu's default repos or on GitHub releases anymore. Built it from
  source per Google's current instructions, added the required
  `kvm`/`cvdnetwork`/`render` groups, and it booted cleanly:
  `sys.boot_completed=1`, a live WebRTC stream, and a real screenshot of a
  genuine two-display AAOS cockpit (head unit + separate instrument
  cluster) — including Google's own real `CarService` placeholder text,
  proof this is authentic framework code, not a mockup.

  **This same app, installed and tested on that self-built automotive OS.**
  Not a separate demo — the actual debug APK from this project, `adb
  install`ed onto the booted `cvd-1` instance and launched explicitly
  (`adb shell am start`, since it's not a declared automotive app — AAOS
  applied its own `0.7x` density compat-scale automatically, a real minor
  detail worth knowing about, not a failure). Three of four pieces
  verified working, one genuine wall found and precisely diagnosed:
  - **AIDL + OpenGL ES:** confirmed via the app's *own* runtime perf log
    firing on real automotive hardware config (`VehicleHud: draw avg: 144
    us, swap avg: 1684 us, over 300 frames`, window bounds
    `Rect(0,0-1080,600)`) — real frames, not just "it launched."
  - **Vulkan:** confirmed via `dumpsys window` genuinely showing
    `mCurrentFocus`/`mFocusedApp` switch to `.VulkanActivity` with no
    crash — the authoritative check, since the WebRTC screenshot alone
    looked visually similar to the GLES view (both are simple green
    shapes on black) and wasn't diagnostic on its own.
  - **UAAL/Unity: a real architectural wall, precisely diagnosed, not a
    code bug.** Long-press launched `UnityPlayerGameActivity`, which
    crashed immediately: `UnsatisfiedLinkError: dlopen failed: library
    "libgame.so" not found`. Root cause: this APK's Unity IL2CPP native
    output is ARM64-only (built for real phones); Cuttlefish is x86_64,
    and Unity's native code is architecture-specific compiled machine
    code, not portable bytecode. Attempted the real fix — added
    `AndroidArchitecture.X86_64` to `PlayerSettings.Android
    .targetArchitectures` in `vehicle-hud-unity`'s build script and ran a
    clean batch-mode rebuild — and got a definitive answer from the
    Editor itself: `CS0618: 'AndroidArchitecture.X86_64' is obsolete:
    'X86_64 is no longer supported.'` Unity 6000.5.9f1 has fully removed
    x86_64 Android support; the setting is a silent no-op. Not fixable
    from this project — a genuine Unity product limitation, verified by
    attempting the fix and reading the compiler's own answer, not assumed.
    Reverted the pointless setting, documented why in the build script,
    and confirmed the export still works normally for its real target
    (ARM64 phones).
  - **What this wall actually means, checked against Unity's own product
    page:** Unity's automotive HMI case studies (Toyota, Mercedes-Benz AG,
    Mazda's 2027 CX-5) run on **Unity Industry + Embedded platform
    support** — a separate enterprise product from the standard Editor
    used here, access-gated per Unity's own FAQ ("contact your account
    manager or the Unity Sales team for access"). That's a real, honest
    boundary — not reproducible from this project, and not worth
    pretending otherwise. But the same FAQ states those embedded runtimes
    target **ARM-based processors**, which reframes the wall above: it's
    specific to Cuttlefish being an **x86_64 virtual device** (an
    artifact of testing on this Intel Mac's host architecture), not proof
    Unity can't do automotive HMI. This project's own Unity/UAAL build is
    already ARM64-only by default — the same architecture real automotive
    silicon uses — and was already verified working via on-device testing
    in Phase 4, on real ARM64 Android hardware, before this AOSP/Cuttlefish
    detour. So: the exact enterprise pipeline Mazda uses is out of reach,
    but the underlying capability (Unity real-time rendering as embedded
    automotive HMI content) already has two independent, real pieces of
    evidence behind it here — ARM64 on-device UAAL, and a self-built,
    booted AAOS system with AIDL/GLES/Vulkan verified live on it — and the
    one wall in between is a test-harness CPU-architecture mismatch, not a
    capability gap.

  **Closing the last two implementation-level gaps: SurfaceFlinger and a
  real HAL, each edited and verified live, not just compiled.** Real
  platform-development inner loop — the fast one (incremental build +
  push + targeted service restart), not another multi-hour full image
  build, since the baseline `sdv_ivi_cf` was already booted.
  - **SurfaceFlinger:** added a real log line to
    `SurfaceFlinger::onCompositionPresented()` (a genuine per-frame
    present-latency marker for the pacesetter display, matching existing
    `.ns()`/`to_string()`/`nanoseconds_to_milliseconds()` conventions
    already used elsewhere in the same file — not a cosmetic no-op).
    Incremental build (`m surfaceflinger`, scoped to 1280 total steps vs.
    78K/194K for a full image): **`Build Succeeded` in 2m23s.** Pushed the
    new binary directly (`adb root && disable-verity && reboot && remount
    && adb push`), restarted just that service
    (`adb shell stop/start surfaceflinger`), and confirmed via `adb
    logcat` that the line fires on every real composed frame from the
    actual running process, with genuine varying present-latency numbers
    (1-3ms) — not stale output.
  - **A real HAL, with an honest correction along the way.** Added a
    write-confirmation log line to `FakeVehicleHardware::setValue()` (the
    real per-property VHAL write path), matching the file's existing
    `PROP_ID_TO_CSTR` logging convention. First deployment attempt guessed
    the wrong binary — `vhal_proxy_server`, a host-side tool that also
    links this same library, but isn't the service actually registered as
    the guest's live `IVehicle/default` — found out by testing, not
    assuming: triggered a real property write and the log never fired.
    `adb shell ps -A` plus reading the real `Android.bp` found the
    genuine serving process,
    `android.hardware.automotive.vehicle@V1-sdv-emulator-service`, which
    statically links the same edited library — confirming the source edit
    was right, only the deployment target was wrong. Rebuilt that correct
    target (46s), pushed it, and restarted it via its real init service
    name pulled from its own `.rc` file (`vendor.sdv-vehicle-hal-emulator`
    — not the AIDL interface name, a genuine gotcha). Verified live: a
    real triggered property write (`dumpsys
    android.hardware.automotive.vehicle.IVehicle/default --set
    HVAC_TEMPERATURE_SET ...`) produced the new log line from the newly
    restarted process — on-device proof, with both this and the
    SurfaceFlinger line firing together in the same `logcat` stream.

- **Phase 8 (done, verified on-device) — multi-screen windowing, GLES and
  Vulkan both routed to secondary displays.** `DisplayTracker` listens for
  `DisplayManager` add/remove events and shows one `Presentation` per
  secondary display; a pluggable `DisplayPolicy` decides whether it hosts the
  GLES `HudSurfaceView` or the `VulkanSurfaceView`. Neither renderer changed:
  each already owns its own render thread and context per surface.
  `TelemetryRouter` fans telemetry out to every HUD surface, and an exported
  `ModeSwitchReceiver` toggles the secondary display between GLES and Vulkan
  from adb. Verified on a Pixel 6a with Developer options → Simulate secondary
  displays (1920×1080). Launching Unity onto the secondary display with
  `setLaunchDisplayId` throws a `SecurityException` (cross-display launches
  need system permissions); it's caught and falls back to the default
  display. Not yet run on the AAOS build.

- **Phase 9 (done, verified via screenshots) — a cockpit HMI scene in
  Unity.** An orbiting 3D car with five world-anchored hotspots projected to
  screen-space labels every frame, plus a bottom nav bar. A bug where the nav
  bar never moved the highlight was found by comparing screenshots and fixed
  (screenshots in the repo root, `unity_hmi_*.png`).

- **Phase 10 (done, verified on AAOS and on-device) — the car stack, frame
  pacing, and a sturdier Vulkan path.**
  - **Speed from the Vehicle HAL.** `CarSpeedSource` subscribes to
    `PERF_VEHICLE_SPEED` through `CarPropertyManager` (app → Car Service →
    Vehicle HAL) on Android Automotive, with the `CAR_SPEED` permission; on a
    phone the app keeps its own AIDL service. On the self-built `sdv_ivi_cf`
    image, driven by the fake VHAL's linear generator (`adb root`, then
    `dumpsys android.hardware.automotive.vehicle.IVehicle/default
    --genfakedata --startlinear 0x11600207 15 0 15 0.5 100000000`), the app
    logged 0 → 3.6 → 21.6 → 39.6 → 57.6 → 75.6 km/h and the needle moved.
  - **Frame pacing: 52 → 60 fps.** The GLES loop slept a fixed 16 ms after
    `eglSwapBuffers`, which already waits for vsync, so each frame took about
    19 ms: about 52 fps on the Pixel 6a's 60 Hz display. Without the sleep:
    59.8–60.2 fps, now logged per 300 frames. A Perfetto trace after the fix
    (352 frames in 5.86 s, interval median 16.69 ms) showed `dequeueBuffer`
    taking 13.95 ms of the 14.71 ms `renderFrame` section: the vsync wait
    moved to the first GL call of the frame, and the real GL work is 0.76 ms.
    22 of 351 intervals were over 20 ms, so `AChoreographer` pacing is the
    next step.
  - **Vulkan: two frames in flight and swapchain rebuild.** Per-slot acquire
    semaphores and fences, a render-finished semaphore per image, and
    `imagesInFlight` tracking. The swapchain is rebuilt (passing
    `oldSwapchain`) on `OUT_OF_DATE`, on `SUBOPTIMAL` when the surface really
    changed, and on `surfaceChanged`; viewport and scissor are dynamic state.
    Tested with `adb shell wm size 1080x1800` and `wm size reset`: rebuilt
    1080×2400 → 1080×1800 → 1080×2400 at about 60 fps.

- **Phase 11 (done, verified on-device) — a Unity native rendering plugin.**
  `unity-plugin/hudgfx.cpp` builds into `libhudgfx.so` (`unity-plugin/build.sh`,
  NDK clang, Unity's `PluginAPI` headers) and goes into the Unity project's
  `Assets/Plugins/Android/libs/arm64-v8a/`. Unity calls `UnityPluginLoad`,
  the plugin registers for graphics device events through `IUnityGraphics`,
  and on Vulkan it reads Unity's own `VkInstance`/`VkPhysicalDevice` through
  `IUnityGraphicsVulkan`. `HudGfxBridge.cs` (in the Unity project) calls
  `GL.IssuePluginEvent` once per frame. Results on the Pixel 6a:
  - With the graphics API on Automatic, Unity picked **OpenGL ES 3**, even
    though the device reports Vulkan 1.4.305 on a Mali-G78 and our own Vulkan
    renderer runs at 60 fps. Unity's docs say it doesn't allow Pixel 6
    devices to use Vulkan by default.
  - Vulkan-only without an allow-list entry: Unity shows "Your device does
    not match the hardware requirements", then "Unable to initialize the
    Unity Engine Graphics API".
  - Vulkan-only with an allow-list entry (`google` / `bluejay` / `Mali-G78`,
    added by `SceneSetup.ConfigureAndroidExportVulkanAndBuild`): Unity starts
    on Vulkan, and the plugin logs `Unity Vulkan device: Mali-G78, API
    1.4.305, queue family 0`.
  - Threads: `UnityPluginLoad` and the device-initialize event run on
    "Unity Main Thread"; render events run on "UnityGfxDeviceWorker", a
    separate thread, on both GLES and Vulkan. That's why plugin GPU work
    goes through `GL.IssuePluginEvent` and not a direct call from C#.
  - About 30 render events per second in both runs (300 every ~10 s). The
    scene doesn't set `Application.targetFrameRate`.
  - Unity's batch build stops the adb server, so reconnect adb before
    installing.
  - **Drawing (added the same day).** The plugin draws a speed dial (7 ticks,
    a needle, a hub; 51 vertices) on top of Unity's frame. The geometry is
    one GLSL file, `unity-plugin/shaders/dial_body.glsl`, shared by both
    paths: each vertex is built from its index, so there's no vertex buffer.
    `build.sh` compiles it to SPIR-V with the NDK's `glslc` for Vulkan, and
    embeds it as source for GLES. (The first version's needle was a slow
    sweep set from C#; see the next two bullets for real speed.)
    - Vulkan: `ConfigureEvent` asks Unity to be inside a render pass when
      the event runs (`EnsureInside`, plus `ModifiesCommandBuffersState`
      because the plugin binds its own pipeline). `CommandRecordingState`
      gives Unity's command buffer, render pass and subpass; the plugin
      builds one pipeline per render pass, then pushes constants and draws.
      Log: pipeline created for Unity's render pass, subpass 0, and the draw
      recorded into a *secondary* command buffer (level 1), 1080×2400.
    - GLES: the same draw with Unity's context on its render thread. GL
      objects are created lazily there (the device-initialize callback runs
      on the main thread, which doesn't own the context), and every state
      the draw changes (program, VAO, viewport, depth, cull, blend, scissor)
      is put back, because Unity caches GL state. `glGetError` 0 after the
      first draw.
    - Screenshots: `unity_plugin_dial_vulkan.png`, `unity_plugin_dial_gles.png`.
      The scene under the dial looks the same on both. A GLES-only export
      method, `ConfigureAndroidExportGlesAndBuild`, tests the GLES path now
      that the Pixel 6a is on the Vulkan allow list.
  - **Needle on the app's vehicle speed (same day).** `UnityDialBridge`
    (Kotlin) is a `TelemetryConsumer`: it loads `libhudgfx` and calls
    `nativeSetSpeedKph` over JNI from the telemetry thread, which stores an
    atomic that the render thread reads (0–180 km/h, the native HUD's full
    scale, eased with a 0.12 s time constant because samples come at about
    10 Hz). The JNI side and Unity's `UnityPluginLoad` log the same address
    for that atomic, so it's one library instance. The speed source used to
    stop in `MainActivity.onStop`, which runs as soon as Unity's activity
    covers it; it now runs while any of the app's activities is started
    (`ActivityLifecycleCallbacks`). Checked by screenshots taken right after
    a speed log line: 59.7 km/h on the 60 tick, 43.6 between 30 and 60. On a
    phone the speed comes from the in-app AIDL service; on AAOS the same
    router is fed by `CarPropertyManager`, but Unity doesn't run on the
    x86_64 Cuttlefish build, so that end-to-end path isn't verified.
  - **Drawing into a Unity texture (same day).** C# creates a 512×512
    `RenderTexture` (`enableRandomWrite`), passes `GetNativeTexturePtr()` to
    the plugin, and shows it on a world-space panel above the car. Event 2
    runs in `Update`, before the scene renders.
    - Vulkan: the first try began the plugin's own render pass. Even with
      `EnsureOutside`, Unity handed over a **secondary** command buffer
      (logged), and `vkCmdBeginRenderPass` needs a primary one. So the
      texture path is a compute shader (`shaders/dial.comp`, same layout as
      the vertex dial, drawn as distance fields): `AccessTexture` moves the
      image to `GENERAL` for a compute-shader write and records the barrier,
      then the plugin binds a storage-image descriptor and dispatches 64×64
      groups into Unity's command buffer. Log: VkImage format 37
      (R8G8B8A8_UNORM), usage 0x9f (includes storage). The first screenshot
      showed the dial upside down; flipping y in the compute pass fixed it.
    - GLES: the vertex dial drawn into the texture through an FBO, with the
      previous framebuffer and clear color restored. `glGetError` 0.
    - Screenshots: `unity_plugin_dial_texture_vulkan.png`,
      `unity_plugin_dial_texture_gles.png` (overlay top left, texture panel
      above the car).
  - **On an Android Automotive image (same evening).** Google's Android
    Automotive emulator image (Android 15, arm64,
    `system-images;android-35-ext15;android-automotive;arm64-v8a`) on the
    Apple Silicon Mac, since Unity 6 only ships ARM64 for Android and the
    self-built Cuttlefish image is x86_64. Unity picked GLES there. The app
    subscribed to `PERF_VEHICLE_SPEED` through `CarPropertyManager`, and the
    speed was set in the emulator's Vehicle HAL with `adb root` and `dumpsys
    android.hardware.automotive.vehicle.IVehicle/default --set 0x11600207 -f
    <m/s>`. 15 m/s showed as 54 km/h on both dials; changing it to 40 m/s
    moved both needles to 144 km/h within 2 s with Unity in front
    (`unity_plugin_aaos_54kmh.png`, `unity_plugin_aaos_144kmh.png`). So the
    whole path runs on a car image: Vehicle HAL → Car Service →
    `CarPropertyManager` → `TelemetryRouter` → JNI → Unity's render thread.
    One fix on the way: the emulator's GLES rejects random-write RGBA8
    textures ("format unsupported for random writes"), so the RenderTexture
    asks for random writes only on Vulkan, which is the only path that needs
    them. Like the Cuttlefish cockpit, this image has a private cluster
    display (display 2) and a private `ClusterOsDouble-VD`. Not done here:
    Unity on Vulkan in the emulator.
  - **On the self-built AAOS image (same night).** The self-built
    `sdv_ivi_cf` Cuttlefish is x86_64, with no native bridge
    (`ro.dalvik.vm.native.bridge=0`, even though the ABI list also names
    arm64-v8a), and Unity 6.5 removed x86_64 Android. Unity 6.0 LTS
    (6000.0.84f1) still builds x86-64 (IL2CPP only; Unity labels it "Magic
    Leap 2"), so a copy of the Unity project, `vehicle-hud-unity-x86`, is
    built with 6.0: ARM64 plus x86_64, GLES only, because the guest renders
    with SwiftShader (`gpu_mode` `guest_swiftshader`). Changes needed for
    the 6.0 copy: drop three 6.5-only modules from `Packages/manifest.json`
    (adaptiveperformance, physicscore2d, vectorgraphics), and run the Gradle
    daemon on JDK 21 (`-Dorg.gradle.java.home=...`). Unity 6.0 compiles
    IL2CPP in a CMake step inside Gradle, and with Android Studio's JDK 25
    that step failed on "A restricted method in java.lang.System has been
    called". `ABI=x86_64 unity-plugin/build.sh` builds the plugin for
    x86_64; `-PunityExportDir=../vehicle-hud-unity-x86/Builds/AndroidExport`
    builds the app against that export. On the image, Unity logged Device
    Model "Google IVI and Software-Defined Vehicle Core", CPU x86_64, OpenGL
    ES 3, and both dials drew (`glGetError` 0). The speed came from the
    image's own Vehicle HAL through `CarPropertyManager`: 15 m/s showed as
    54 km/h and 40 m/s as 144 km/h on both dials
    (`unity_plugin_sdv_ivi_cf_54kmh.png`, `unity_plugin_sdv_ivi_cf_144kmh.png`).
    Build for that image:

    ```
    J21=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
    ABI=x86_64 UNITY_PROJECT=../vehicle-hud-unity-x86 unity-plugin/build.sh
    # Unity 6000.0.84f1: -executeMethod SceneSetup.ConfigureAndroidExportX86AndBuild
    JAVA_HOME="$J21" ./gradlew :app:assembleDebug "-Dorg.gradle.java.home=$J21" \
        "-Porg.gradle.java.installations.paths=$J21" \
        -PunityExportDir=../vehicle-hud-unity-x86/Builds/AndroidExport
    ```

## Build

```
./gradlew :app:assembleDebug
```

Requires the Android SDK with NDK `28.2.13676358` and CMake `3.22.1`. Gradle
is pinned to Android Studio's bundled JBR (JDK 21) via
`gradle.properties` (`org.gradle.java.home`), so the CLI and the IDE always
build with the same JVM.

Note (2026-10-12): Android Studio's bundled JBR is now JDK 25, so the JDK 21
toolchain isn't found automatically. Build with a JDK 21 install, for example
Homebrew's `openjdk@21`:

```
J21=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
JAVA_HOME="$J21" ./gradlew :app:assembleDebug "-Porg.gradle.java.installations.paths=$J21"
```
