package dev.yosukesakurai.vehiclehud

import android.os.Bundle
import androidx.activity.ComponentActivity

/** Phase 3: the Vulkan screen, isolated from the GLES HUD in MainActivity. Started as a triangle; now a live speed dial. */
class VulkanActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(VulkanSurfaceView(this))
    }
}
