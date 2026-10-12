package dev.yosukesakurai.vehiclehud

import android.car.Car
import android.car.VehiclePropertyIds
import android.car.hardware.CarPropertyValue
import android.car.hardware.property.CarPropertyManager
import android.content.Context
import android.content.pm.PackageManager
import android.util.Log

/**
 * Reads vehicle speed from the Vehicle HAL through Car Service, on Android
 * Automotive only:
 *
 *   CarPropertyManager -> Binder -> CarService -> VHAL (AIDL) -> vendor impl
 *
 * PERF_VEHICLE_SPEED arrives in m/s and is converted to km/h for the HUD.
 * Only construct this after checking FEATURE_AUTOMOTIVE: on a phone the
 * android.car library isn't present, and the caller keeps using the in-app
 * AIDL TelemetryService instead.
 */
class CarSpeedSource(
    private val context: Context,
    private val onSpeedKph: (Float) -> Unit,
) {
    private var car: Car? = null
    private var propertyManager: CarPropertyManager? = null
    private var eventCount = 0

    private val callback = object : CarPropertyManager.CarPropertyEventCallback {
        override fun onChangeEvent(value: CarPropertyValue<*>) {
            val metersPerSecond = value.value as? Float ?: return
            val kph = metersPerSecond * 3.6f
            // Every 10th event, so logcat shows the VHAL values without
            // flooding at the subscription rate.
            if (eventCount++ % 10 == 0) {
                Log.i(TAG, "VHAL PERF_VEHICLE_SPEED: %.2f m/s = %.1f km/h".format(metersPerSecond, kph))
            }
            onSpeedKph(kph)
        }

        override fun onErrorEvent(propertyId: Int, areaId: Int) {
            Log.w(TAG, "Speed property error: prop=$propertyId area=$areaId")
        }
    }

    /** Returns false if Car Service or the permission isn't available. */
    fun start(): Boolean {
        if (context.checkSelfPermission(Car.PERMISSION_SPEED) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "CAR_SPEED permission not granted")
            return false
        }
        val car = Car.createCar(context) ?: return false
        val manager = car.getCarManager(Car.PROPERTY_SERVICE) as? CarPropertyManager
        if (manager == null) {
            car.disconnect()
            return false
        }
        val registered = manager.registerCallback(
            callback,
            VehiclePropertyIds.PERF_VEHICLE_SPEED,
            CarPropertyManager.SENSOR_RATE_FAST,
        )
        if (!registered) {
            Log.w(TAG, "Could not subscribe to PERF_VEHICLE_SPEED")
            car.disconnect()
            return false
        }
        this.car = car
        propertyManager = manager
        Log.i(TAG, "Subscribed to PERF_VEHICLE_SPEED through CarPropertyManager")
        return true
    }

    fun stop() {
        propertyManager?.unregisterCallback(callback)
        propertyManager = null
        car?.disconnect()
        car = null
    }

    companion object {
        private const val TAG = "CarSpeedSource"
        const val PERMISSION_SPEED = "android.car.permission.CAR_SPEED"
    }
}
