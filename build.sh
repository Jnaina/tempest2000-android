#!/bin/sh
# Builds the debug APK (installable straight away) or, with `release`, an optimised one signed with the debug key.
#   ./build.sh            ->  Tempest2000-debug.apk
#   ./build.sh release    ->  Tempest2000-release.apk
# Works on macOS, Linux and Windows (Git Bash or WSL2). Needs JDK 17 and the Android SDK (see README.md); finds them in the usual
# places if ANDROID_HOME / JAVA_HOME are not set.
set -e
cd "$(dirname "$0")"
[ -f app/src/main/assets/t2000.abs ] && [ -d deps/virtualjaguar-libretro ] || ./setup.sh

if [ -z "$ANDROID_HOME" ]; then
    ANDROID_HOME=${ANDROID_SDK_ROOT:-}
    for d in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk" "${LOCALAPPDATA:-/nonexistent}/Android/Sdk"; do
        [ -z "$ANDROID_HOME" ] && [ -d "$d" ] && ANDROID_HOME=$d
    done
fi
[ -d "$ANDROID_HOME" ] || { echo "build.sh: Android SDK not found. Install it (README.md, step 1) or set ANDROID_HOME."; exit 1; }
export ANDROID_HOME

if [ -z "$JAVA_HOME" ]; then
    for j in /opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home /usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
             /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/temurin-17-jdk-amd64 /usr/lib/jvm/java-17; do
        [ -z "$JAVA_HOME" ] && [ -d "$j" ] && JAVA_HOME=$j
    done
    if [ -z "$JAVA_HOME" ] && [ -x /usr/libexec/java_home ]; then JAVA_HOME=$(/usr/libexec/java_home -v 17 2>/dev/null || true); fi
fi
if [ -n "$JAVA_HOME" ]; then export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"; fi
command -v java >/dev/null 2>&1 || { echo "build.sh: Java not found. Install JDK 17 (README.md, step 1) or set JAVA_HOME."; exit 1; }

if [ "$1" = "release" ]; then ./gradlew --no-daemon assembleRelease && cp app/build/outputs/apk/release/app-release.apk Tempest2000-release.apk && echo "built Tempest2000-release.apk"
else ./gradlew --no-daemon assembleDebug && cp app/build/outputs/apk/debug/app-debug.apk Tempest2000-debug.apk && echo "built Tempest2000-debug.apk"; fi
