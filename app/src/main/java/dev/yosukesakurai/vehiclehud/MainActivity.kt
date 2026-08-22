package dev.yosukesakurai.vehiclehud

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import androidx.activity.ComponentActivity
import kotlin.concurrent.thread

class MainActivity : ComponentActivity() {

    private lateinit var hudView: HudSurfaceView

    private var telemetryBinder: ITelemetryService? = null
    private var pollThread: Thread? = null
    @Volatile private var polling = false

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, service: IBinder) {
            Log.i(TAG, "Connected to TelemetryService")
            telemetryBinder = ITelemetryService.Stub.asInterface(service)
            startPolling()
        }

        override fun onServiceDisconnected(name: ComponentName) {
            Log.i(TAG, "Disconnected from TelemetryService")
            stopPolling()
            telemetryBinder = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hudView = HudSurfaceView(this)
        // Phase 3: tap the HUD to open the separate Vulkan triangle
        // activity. Phase 4: long-press to open the embedded Unity view
        // (UAAL) instead — explicit component name, since
        // UnityPlayerGameActivity is declared in unityLibrary's own
        // manifest, not one this project owns. No layout XML in this
        // project, so click listeners on the existing view are the
        // minimal way to add entry points rather than introducing a new
        // UI framework just for this.
        hudView.isClickable = true
        hudView.setOnClickListener {
            startActivity(Intent(this, VulkanActivity::class.java))
        }
        hudView.setOnLongClickListener {
            startActivity(Intent().setClassName(this, "com.unity3d.player.UnityPlayerGameActivity"))
            true
        }
        setContentView(hudView)
    }

    override fun onStart() {
        super.onStart()
        bindService(Intent(this, TelemetryService::class.java), connection, Context.BIND_AUTO_CREATE)
    }

    override fun onStop() {
        stopPolling()
        unbindService(connection)
        super.onStop()
    }

    private fun startPolling() {
        polling = true
        pollThread = thread(name = "TelemetryAidlPoll") {
            // Deliberately its own thread, never the UI thread and never
            // the native render thread: a synchronous Binder call blocks
            // the calling thread for the IPC round-trip, so it must not
            // be either of those two.
            while (polling) {
                val binder = telemetryBinder
                if (binder != null) {
                    try {
                        val speed = binder.getSpeedKph()
                        val rpm = binder.getRpm()
                        hudView.pushExternalTelemetry(speed, rpm)
                    } catch (e: Exception) {
                        Log.e(TAG, "AIDL call to TelemetryService failed", e)
                    }
                }
                Thread.sleep(100)
            }
        }
    }

    private fun stopPolling() {
        polling = false
        pollThread?.join(500)
        pollThread = null
    }

    companion object {
        private const val TAG = "VehicleHud"
    }
}
