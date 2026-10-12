package dev.yosukesakurai.vehiclehud

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper

/**
 * Exported manifest receiver that lets adb (or another debug tool) toggle the
 * secondary display mode. The actual state lives in MainActivity; this class
 * just forwards the request through a weak callback pattern so it works even
 * when the Activity is recreated.
 *
 * The callback is posted to the main looper rather than invoked synchronously
 * so the receiver returns before any window/Presentation work happens.
 */
class ModeSwitchReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_SWITCH_SECONDARY_DISPLAY) return
        val cb = callback ?: return
        Handler(Looper.getMainLooper()).post { cb.invoke() }
    }

    companion object {
        const val ACTION_SWITCH_SECONDARY_DISPLAY =
            "dev.yosukesakurai.vehiclehud.SWITCH_SECONDARY_DISPLAY"

        @Volatile var callback: (() -> Unit)? = null
    }
}
