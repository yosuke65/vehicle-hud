package dev.yosukesakurai.vehiclehud

import android.app.Activity
import android.app.ActivityOptions
import android.app.Application
import android.car.Car
import android.car.CarOccupantZoneManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.IBinder
import android.os.RemoteException
import android.util.Log
import android.view.Display
import android.widget.Toast
import androidx.activity.ComponentActivity

class MainActivity : ComponentActivity() {

    private lateinit var hudView: HudSurfaceView

    private var telemetryService: ITelemetryService? = null
    private var aidlBound = false

    // On Android Automotive, speed comes from the Vehicle HAL through
    // CarPropertyManager instead of the in-app AIDL service.
    private var carSpeedSource: CarSpeedSource? = null
    private val isAutomotive by lazy {
        packageManager.hasSystemFeature(PackageManager.FEATURE_AUTOMOTIVE)
    }

    // Process-wide, so the Vulkan screen and Unity get the same stream.
    private val telemetryRouter = TelemetryRouter.shared

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

    // For the restart measurement: when the service died, and whether the
    // first sample from the restarted service has arrived yet.
    @Volatile private var serviceDiedAtNanos = 0L
    @Volatile private var awaitingFirstSampleAfterRestart = false

    // The :telemetry process pushes samples through this oneway callback, so
    // nothing in this process polls or blocks on a Binder call. Calls arrive
    // on this process's Binder threads; oneway calls to one binder object are
    // queued and delivered one at a time, in order.
    private val telemetryListener = object : ITelemetryListener.Stub() {
        override fun onTelemetry(speedKph: Float, rpm: Float) {
            if (awaitingFirstSampleAfterRestart) {
                awaitingFirstSampleAfterRestart = false
                val ms = (System.nanoTime() - serviceDiedAtNanos) / 1_000_000
                Log.i(TAG, "First sample from the restarted TelemetryService, $ms ms after binderDied")
            }
            telemetryRouter.push(speedKph, rpm)
        }
    }

    // Runs on a Binder thread when the :telemetry process dies. The binding
    // uses BIND_AUTO_CREATE, so the system restarts the service, and
    // onServiceConnected runs again with a new binder.
    private val serviceDeath = IBinder.DeathRecipient {
        serviceDiedAtNanos = System.nanoTime()
        awaitingFirstSampleAfterRestart = true
        telemetryService = null
        Log.w(TAG, "TelemetryService died (binderDied); waiting for the system to restart it")
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, service: IBinder) {
            val telemetry = ITelemetryService.Stub.asInterface(service)
            try {
                service.linkToDeath(serviceDeath, 0)
                telemetry.registerListener(telemetryListener)
            } catch (e: RemoteException) {
                Log.e(TAG, "TelemetryService died while connecting", e)
                return
            }
            telemetryService = telemetry
            Log.i(TAG, "Connected to TelemetryService; registered for oneway callbacks")
        }

        override fun onServiceDisconnected(name: ComponentName) {
            Log.i(TAG, "Disconnected from TelemetryService")
            telemetryService = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hudView = HudSurfaceView(this)
        telemetryRouter.add(hudView)
        telemetryRouter.add(UnityDialBridge)
        // Registered before onStart, so this activity's own start counts.
        application.registerActivityLifecycleCallbacks(speedLifecycle)

        // Phase 3: tap the HUD to open the separate Vulkan activity (now a
        // live speed dial). Phase 4: long-press to open the embedded Unity
        // view (UAAL) instead — explicit component name, since
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
        telemetryService?.let { telemetry ->
            try {
                telemetry.unregisterListener(telemetryListener) // oneway: doesn't block this thread
                telemetry.asBinder().unlinkToDeath(serviceDeath, 0)
            } catch (e: RemoteException) {
                // The service is already gone; nothing to unregister from.
            } catch (e: NoSuchElementException) {
                // Not linked any more (it died and was not reconnected yet).
            }
        }
        telemetryService = null
        if (aidlBound) {
            unbindService(connection)
            aidlBound = false
        }
    }

    override fun onDestroy() {
        application.unregisterActivityLifecycleCallbacks(speedLifecycle)
        if (startedActivities > 0) stopSpeedSource()
        telemetryRouter.remove(hudView)
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
        // On a car, the driver's instrument cluster comes first.
        val displayId = clusterDisplayId() ?: displayTracker.firstDisplayId()
        if (displayId != null) {
            try {
                val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
                startActivity(Intent(intent).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), options.toBundle())
                Log.i(TAG, "Launched Unity on display $displayId")
                return
            } catch (e: SecurityException) {
                // Stock Android only lets an app launch onto a private display
                // if it holds INTERNAL_SYSTEM_WINDOW (platform signature) or
                // owns the display. Fall back to the default display rather
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

    /**
     * The driver's instrument-cluster display on Android Automotive, or null.
     * It's a private display, so DisplayManager doesn't list it for normal
     * apps. The AAOS way to find it is CarOccupantZoneManager's
     * getDisplayIdForDriver(), a @SystemApi that needs ACCESS_PRIVATE_DISPLAY_ID
     * (signature|privileged), so this only works when the HUD is installed as
     * a privileged app. Called by reflection because the public car library
     * doesn't expose system APIs.
     */
    private fun clusterDisplayId(): Int? {
        if (!isAutomotive ||
            checkSelfPermission(PERMISSION_ACCESS_PRIVATE_DISPLAY_ID) != PackageManager.PERMISSION_GRANTED
        ) {
            return null
        }
        val car = Car.createCar(this) ?: return null
        return try {
            val zones = car.getCarManager(Car.CAR_OCCUPANT_ZONE_SERVICE) as? CarOccupantZoneManager
            val id = zones?.let {
                CarOccupantZoneManager::class.java
                    .getMethod("getDisplayIdForDriver", Int::class.javaPrimitiveType)
                    .invoke(it, CarOccupantZoneManager.DISPLAY_TYPE_INSTRUMENT_CLUSTER) as Int
            }
            Log.i(TAG, "Driver instrument-cluster display: $id")
            id?.takeIf { it != Display.INVALID_DISPLAY }
        } catch (e: ReflectiveOperationException) {
            Log.w(TAG, "Cluster display lookup failed", e)
            null
        } finally {
            car.disconnect()
        }
    }

    companion object {
        private const val TAG = "VehicleHud"
        private const val REQUEST_CAR_SPEED = 1
        private const val PERMISSION_ACCESS_PRIVATE_DISPLAY_ID = "android.car.permission.ACCESS_PRIVATE_DISPLAY_ID"
    }
}
