@file:Suppress("UnstableApiUsage")

pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
// Phase 4 (UAAL): unityLibrary lives in the separate vehicle-hud-unity
// project's export output, not copied into this repo — referenced in
// place so re-exporting from Unity picks up here without a manual copy.
val unityExportDir = file("../vehicle-hud-unity/Builds/AndroidExport")

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // unityLibrary/build.gradle bundles some dependencies as plain
        // .jar files under libs/, resolved via this flatDir repo — same
        // configuration Unity's own exported settings.gradle uses.
        flatDir {
            dirs("$unityExportDir/unityLibrary/libs")
        }
    }
}

rootProject.name = "Vehicle HUD"
include(":app")
include(":unityLibrary")
project(":unityLibrary").projectDir = file("$unityExportDir/unityLibrary")
