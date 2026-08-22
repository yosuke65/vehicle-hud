plugins {
    alias(libs.plugins.android.application) apply false
    // Needed by the merged-in unityLibrary module (Phase 4, UAAL) — a
    // com.android.library module, exported by Unity, that this project
    // doesn't own but does need to declare the plugin classpath for.
    alias(libs.plugins.android.library) apply false
}
