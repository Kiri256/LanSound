plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "local.lanaudio"
    compileSdk = 35

    defaultConfig {
        applicationId = "local.lanaudio"
        minSdk = 26
        targetSdk = 35
        versionCode = 126
        versionName = "1.2.6"
        val packetFrames = providers.gradleProperty("lanaudio.packetFrames").orNull ?: "96"
        require(packetFrames in listOf("96", "192", "240")) { "lanaudio.packetFrames must be 96, 192, or 240" }
        buildConfigField("int", "PACKET_FRAMES", packetFrames)
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-O3", "-DLANAUDIO_PACKET_FRAMES=$packetFrames")
                // The Maven Oboe binaries require the shared C++ runtime.
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }

    buildFeatures {
        buildConfig = true
        prefab = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.9.3")
}
