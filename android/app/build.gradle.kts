plugins {
    alias(libs.plugins.android.application)
}

val cpmSourceCacheDir = System.getenv("HOME") + "/.cache/CPM/uapmd"

val aapDir = project.projectDir.parentFile.listFiles {
    it.name == "external" }.firstOrNull()?.listFiles { it.name == "aap-core" }?.firstOrNull()

android {
    namespace = "dev.atsushieno.uapmd"
    compileSdk {
        version = release(libs.versions.androidTargetSdk.get().toInt()) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "dev.atsushieno.uapmd"
        minSdk = libs.versions.androidMinSdk.get().toInt() // libremidi kinda expects AMIDI
        targetSdk = libs.versions.androidTargetSdk.get().toInt()
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                arguments.addAll(listOf(
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DAAP_DIR=$aapDir",
                    "-DMIDICCI_SKIP_TOOLS=ON",
                    "-DCPM_SOURCE_CACHE=$cpmSourceCacheDir",
                    "-DANDROID_STL=c++_shared"  // Required for C++23 support
                ))
                targets.add("main")
                cppFlags.add("-std=c++23")
                cppFlags.add("-fexceptions")
            }
        }
        ndk.abiFilters.addAll(listOf("arm64-v8a", "x86_64"))
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
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
    buildFeatures {
        prefab = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.startup.runtime)
    implementation(libs.material)
    implementation(files("../external/SDL3-3.4.0.aar"))
    implementation(libs.androidaudioplugin)
    implementation(libs.androidaudioplugin.manager)
    implementation(libs.oboe)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}
