// Every plugin used by a subproject is declared here once, so all subprojects share one build
// classpath (and one Kotlin Gradle plugin version).
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.jvm) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
