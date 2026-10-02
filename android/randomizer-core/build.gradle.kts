plugins {
    `java-library`
}

java {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
}

// This module is a git subtree of a trimmed, Android-compatible fork of
// the Universal Pokemon Randomizer ZX (see UPGRADING.md for the fork/sync
// workflow) -- its own source layout is the upstream project's own
// (`src/com/dabomstew/...`), not Gradle's usual `src/main/java/...`
// convention. Pointing both the java and resources source sets at the
// same `src` directory, rather than moving files to fit Gradle's
// convention, keeps this directory a clean 1:1 mirror of the fork for
// future `git subtree pull`s -- resource files (ROM offset tables, IPS
// patches, UI strings) live interspersed with the .java files in that
// same tree, same as upstream has them.
sourceSets {
    main {
        java.srcDirs("src")
        resources.srcDirs("src")
        resources.exclude("**/*.java")
    }
}

// Deliberately a plain `java-library` module, not a com.android.library
// one: compiling against a real JDK (not Android's stripped API surface)
// is what let the fork's source compile unmodified for anything outside
// the handful of actual AWT/Swing call sites already removed on the fork
// side -- see that repo's android-compatibility branch for the full story.
