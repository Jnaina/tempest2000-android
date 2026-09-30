#!/bin/sh
# Runs the app in the Android emulator on this computer (macOS, Linux, Windows/Git Bash; the emulator needs hardware virtualisation).
#   ./emulator.sh              open the emulator window, install the APK and start the game
#   ./emulator.sh --headless   same without a window (for automated tests; use `adb exec-out screencap -p > shot.png` to look)
#   ./emulator.sh --stop       shut the emulator down
# The first run creates a virtual Pixel 6 called t2k_pixel (set AVD_NAME to use another name).
# In the window the Mac keyboard works: arrow keys / WASD move, Space or X = fire, Z = jump, C or V = superzapper,
# Enter or O = option, P = pause; mouse clicks press the on-screen buttons.
set -e
cd "$(dirname "$0")"
case "$(uname -s)" in Darwin) OS=mac;; Linux) OS=linux;; *) OS=windows;; esac
if [ -z "$ANDROID_HOME" ]; then
    ANDROID_HOME=${ANDROID_SDK_ROOT:-}
    for d in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk" "${LOCALAPPDATA:-/nonexistent}/Android/Sdk"; do
        [ -z "$ANDROID_HOME" ] && [ -d "$d" ] && ANDROID_HOME=$d
    done
fi
[ -d "$ANDROID_HOME" ] || { echo "emulator.sh: Android SDK not found (README.md, step 1) - set ANDROID_HOME."; exit 1; }
export ANDROID_HOME
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
if [ -z "$JAVA_HOME" ]; then
    for j in /opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home /usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
             /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/temurin-17-jdk-amd64 /usr/lib/jvm/java-17; do
        [ -z "$JAVA_HOME" ] && [ -d "$j" ] && JAVA_HOME=$j
    done
fi
if [ -n "$JAVA_HOME" ]; then export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"; fi
AVD=${AVD_NAME:-t2k_pixel}
# the emulator runs the image matching this computer's CPU (Apple silicon: arm64; Intel/AMD: x86_64)
case "$(uname -m)" in arm64|aarch64) ABI=arm64-v8a;; *) ABI=x86_64;; esac
IMAGE="system-images;android-34;default;$ABI"
LOG=${TMPDIR:-/tmp}/t2k-emulator.log

[ "$1" = "--stop" ] && { adb emu kill 2>/dev/null || true; exit 0; }
command -v emulator >/dev/null || { echo "Android emulator not found under $ANDROID_HOME - see README.md (sdkmanager \"emulator\" \"$IMAGE\")"; exit 1; }
[ -f Tempest2000-debug.apk ] || ./build.sh

# the virtual phone, created once
if ! emulator -list-avds | grep -qx "$AVD"; then
    [ -d "$ANDROID_HOME/system-images/android-34/default/$ABI" ] || sdkmanager --install "$IMAGE"
    echo no | avdmanager create avd -n "$AVD" -k "$IMAGE" -d pixel_6 --force >/dev/null
    CFG="$HOME/.android/avd/$AVD.avd/config.ini"
    for kv in hw.keyboard=yes hw.ramSize=2048 disk.dataPartition.size=3072M hw.cpu.ncore=4 hw.initialOrientation=landscape showDeviceFrame=no; do
        grep -v "^${kv%%=*}=" "$CFG" > "$CFG.new" || true; echo "$kv" >> "$CFG.new"; mv "$CFG.new" "$CFG"
    done
    echo "created virtual device $AVD"
fi

# start it (unless one is already running)
if ! adb devices | grep -q "^emulator"; then
    GPU=host
    if [ "$OS" = linux ]; then
        [ -w /dev/kvm ] || echo "warning: /dev/kvm is not usable - the emulator will be far too slow. Enable virtualisation and add yourself to the kvm group (README.md)."
        [ "$1" = "--headless" ] && GPU=swiftshader_indirect
    fi
    ARGS="-avd $AVD -no-snapshot -no-boot-anim -gpu $GPU"
    [ "$1" = "--headless" ] && ARGS="$ARGS -no-window -no-audio"
    nohup emulator $ARGS > "$LOG" 2>&1 &
    echo "starting the emulator ..."
fi
i=0; until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do i=$((i+1)); [ $i -gt 120 ] && { echo "emulator did not boot (see $LOG)"; exit 1; }; sleep 2; done
adb shell settings put secure immersive_mode_confirmations confirmed
adb install -r Tempest2000-debug.apk
adb shell am start -n com.jnaina.tempest2000/.MainActivity
echo "Tempest 2000 is running in the emulator. Log lines: adb logcat -s T2K"
