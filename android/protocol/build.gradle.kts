import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.kotlin.jvm)
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    testImplementation(platform(libs.junit.bom))
    testImplementation(libs.junit.jupiter)
    testRuntimeOnly(libs.junit.platform.launcher)
}

// The golden test vectors live at the repository root (protocol/test-vectors), shared with the
// TypeScript host and the Swift package.
val repositoryRoot: String = rootProject.projectDir.parentFile.absolutePath

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
    systemProperty("mwb.repoRoot", repositoryRoot)
    inputs.dir("$repositoryRoot/protocol/test-vectors").withPropertyName("testVectors")
}
