package dev.yosukesakurai.vehiclehud

import android.os.Bundle
import androidx.activity.ComponentActivity

/** Phase 3: a minimal Vulkan triangle, isolated from the GLES HUD in MainActivity. */
class VulkanActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(VulkanSurfaceView(this))
    }
}
