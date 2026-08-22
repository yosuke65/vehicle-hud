plugins {
    alias(libs.plugins.android.application)
    // AGP 9's Kotlin support is now built in — applying the separate
    // org.jetbrains.kotlin.android plugin alongside it is a hard error
    // ("no longer required for Kotlin support since AGP 9.0").
}

android {
    namespace = "dev.yosukesakurai.vehiclehud"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "dev.yosukesakurai.vehiclehud"
        // Bumped from 24: Unity 6's exported unityLibrary (Phase 4, UAAL)
        // hard-requires minSdk 26 — its own build.gradle sets minSdk 26
        // directly, and a lower value here would conflict at merge time.
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
    kotlin {
        jvmToolchain(21)
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        aidl = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity)
    // Phase 4 (UAAL): the Unity-exported Gradle library module, referenced
    // in place from ../vehicle-hud-unity (see settings.gradle.kts).
    implementation(project(":unityLibrary"))

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}
