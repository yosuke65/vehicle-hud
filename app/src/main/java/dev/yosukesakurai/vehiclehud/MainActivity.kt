package dev.yosukesakurai.vehiclehud

import android.app.Activity
import android.app.ActivityOptions
import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import kotlin.concurrent.thread

class MainActivity : ComponentActivity() {

    private lateinit var hudView: HudSurfaceView

    private var telemetryBinder: ITelemetryService? = null
    private var pollThread: Thread? = null
    @Volatile private var polling = false
    private var aidlBound = false

    // On Android Automotive, speed comes from the Vehicle HAL through
    // CarPropertyManager instead of the in-app AIDL service.
    private var carSpeedSource: CarSpeedSource? = null
    private val isAutomotive by lazy {
        packageManager.hasSystemFeature(PackageManager.FEATURE_AUTOMOTIVE)
    }

    private val telemetryRouter = TelemetryRouter()

    // Speed keeps flowing while any of this app's activities is started, not
    // just this one: Unity's UnityPlayerGameActivity covers MainActivity and
    // draws the speed dial through libhudgfx, so stopping in our own onStop
    // would freeze the needle the moment Unity opens.
    private var startedActivities = 0
    private val speedLifecycle = object : Application.ActivityLifecycleCallbacks {
        override fun onActivityStarted(activity: Activity) {
            if (startedActivities++ == 0) startSpeedSource()
        }

        override fun onActivityStopped(activity: Activity) {
            if (--startedActivities == 0) stopSpeedSource()
        }

        override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}
        override fun onActivityResumed(activity: Activity) {}
        override fun onActivityPaused(activity: Activity) {}
        override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
        override fun onActivityDestroyed(activity: Activity) {}
    }
    private var secondaryDisplayMode = SecondaryDisplayMode.HUD

    private val displayTracker by lazy {
        DisplayTracker(this, createPolicyForMode(secondaryDisplayMode))
    }

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
        telemetryRouter.add(hudView)
        telemetryRouter.add(UnityDialBridge)
        // Registered before onStart, so this activity's own start counts.
        application.registerActivityLifecycleCallbacks(speedLifecycle)

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
            launchUnityOnSecondaryDisplay()
            true
        }
        setContentView(hudView)

        ModeSwitchReceiver.callback = { toggleSecondaryDisplayMode() }

        if (isAutomotive &&
            checkSelfPermission(CarSpeedSource.PERMISSION_SPEED) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(CarSpeedSource.PERMISSION_SPEED), REQUEST_CAR_SPEED)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_CAR_SPEED &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            // Restart so onStart() picks the Vehicle HAL source.
            recreate()
        }
    }

    private fun startSpeedSource() {
        if (isAutomotive) {
            val source = CarSpeedSource(this) { kph -> telemetryRouter.push(kph, 0f) }
            if (source.start()) {
                carSpeedSource = source
                Log.i(TAG, "Speed source: Vehicle HAL via CarPropertyManager")
                return
            }
        }
        Log.i(TAG, "Speed source: in-app AIDL TelemetryService")
        aidlBound = bindService(Intent(this, TelemetryService::class.java), connection, Context.BIND_AUTO_CREATE)
    }

    private fun stopSpeedSource() {
        carSpeedSource?.stop()
        carSpeedSource = null
        stopPolling()
        if (aidlBound) {
            unbindService(connection)
            aidlBound = false
        }
    }

    override fun onDestroy() {
        application.unregisterActivityLifecycleCallbacks(speedLifecycle)
        if (startedActivities > 0) stopSpeedSource()
        ModeSwitchReceiver.callback = null
        super.onDestroy()
    }

    private fun toggleSecondaryDisplayMode() {
        secondaryDisplayMode = when (secondaryDisplayMode) {
            SecondaryDisplayMode.HUD -> SecondaryDisplayMode.VULKAN
            SecondaryDisplayMode.VULKAN -> SecondaryDisplayMode.HUD
        }
        displayTracker.setPolicy(createPolicyForMode(secondaryDisplayMode))
        runOnUiThread {
            Toast.makeText(
                this@MainActivity,
                "Secondary display mode: $secondaryDisplayMode",
                Toast.LENGTH_SHORT
            ).show()
        }
    }

    override fun onStart() {
        super.onStart()
        displayTracker.start()
        if (!displayTracker.hasSecondaryDisplay()) {
            Toast.makeText(
                this,
                "No secondary display. Enable Developer options -> Simulate secondary displays to test.",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    override fun onStop() {
        displayTracker.stop()
        super.onStop()
    }

    private fun createPolicyForMode(mode: SecondaryDisplayMode): DisplayPolicy = when (mode) {
        SecondaryDisplayMode.HUD -> HudPresentationPolicy(this, telemetryRouter)
        SecondaryDisplayMode.VULKAN -> VulkanPresentationPolicy(this)
    }

    private fun launchUnityOnSecondaryDisplay() {
        val intent = Intent().setClassName(this, "com.unity3d.player.UnityPlayerGameActivity")
        val displayId = displayTracker.firstDisplayId()
        if (displayId != null) {
            try {
                val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
                startActivity(intent, options.toBundle())
                return
            } catch (e: SecurityException) {
                // Stock Android restricts launching Activities on a secondary
                // display to apps holding system permissions (e.g.
                // ACTIVITY_EMBEDDING). Fall back to the default display rather
                // than crashing the host app.
                Log.w(TAG, "Cannot launch Unity on display $displayId: ${e.message}")
                Toast.makeText(
                    this,
                    "Secondary-display launch denied; falling back to default display",
                    Toast.LENGTH_SHORT
                ).show()
            }
        }
        startActivity(intent)
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
                        telemetryRouter.push(speed, rpm)
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
        private const val REQUEST_CAR_SPEED = 1
    }
}
