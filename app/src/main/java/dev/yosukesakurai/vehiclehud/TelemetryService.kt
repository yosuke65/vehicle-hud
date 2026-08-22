package dev.yosukesakurai.vehiclehud

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import kotlin.concurrent.thread
import kotlin.math.sin

/**
 * Runs in a separate process (see android:process=":telemetry" in the
 * manifest), so binding to it is a genuine cross-process Binder call, not
 * an in-process interface call. Generates its own fake driving-cycle
 * telemetry, independent of the native TelemetrySource — a real HAL
 * client in this position wouldn't know or care that anything else also
 * has its own generator; it only sees whatever this service reports.
 */
class TelemetryService : Service() {

    private val startTime = System.nanoTime()

    // Binder calls land on a pool thread the OS manages for this process,
    // not necessarily the thread that's writing these — @Volatile is
    // enough here since each field is a single primitive read/write, not
    // a multi-field transaction that needs a lock.
    //
    // Deliberately NOT named speedKph/rpm: the AIDL-generated
    // ITelemetryService interface has getSpeedKph()/getRpm() methods, and
    // Kotlin synthesizes a matching property from any Java getter — inside
    // the anonymous `binder` object below (which implements that
    // interface), an unqualified `speedKph` resolves to THAT synthetic
    // property on `this`, not to a same-named outer-class field, which
    // shadows it. Learned this the hard way: naming the getter's own
    // return expression the same as the getter's synthesized property
    // recurses into itself and StackOverflows the binder thread.
    @Volatile private var currentSpeedKph = 0f
    @Volatile private var currentRpm = 0f

    @Volatile private var running = false

    private val binder = object : ITelemetryService.Stub() {
        override fun getSpeedKph(): Float = currentSpeedKph
        override fun getRpm(): Float = currentRpm
    }

    override fun onCreate() {
        super.onCreate()
        running = true
        thread(name = "TelemetryServiceGenerator") {
            while (running) {
                val elapsed = (System.nanoTime() - startTime) / 1_000_000_000f
                // Different phase/rate from the native generator on
                // purpose, so it's visually obvious once this source
                // takes over the needle instead of the native fake one.
                currentSpeedKph = (sin(elapsed * 0.6f) * 0.5f + 0.5f) * 180f
                currentRpm = (sin(elapsed * 1.3f + 0.5f) * 0.5f + 0.5f) * 6000f
                Thread.sleep(100) // ~10Hz, matches the native cadence
            }
        }
        Log.i(TAG, "TelemetryService created in pid=${android.os.Process.myPid()}")
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    companion object {
        private const val TAG = "VehicleHud"
    }
}
