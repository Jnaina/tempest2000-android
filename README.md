# Tempest 2000 for Android

Jeff Minter's original **Tempest 2000** (Atari Jaguar, 1994) as an Android app. The original source is assembled unchanged
(checked to be **byte-for-byte identical to the 1994 build**) and runs on the open-source Virtual Jaguar emulation core, compiled
with the NDK into one small native library. This repository contains only the Android app and build scripts; `./setup.sh`
downloads Jeff's source and the emulator core at pinned versions and builds the game. The game file is never stored in git.

* **APK:** about 4 MB, Android 8.0 (API 26)+, ABIs `arm64-v8a` (phones, tablets, Android TV, Apple-silicon emulator) and `x86_64` (emulators)
* **Controls:** on-screen touch controls, game controllers, keyboard, or TV remote
* **Build hosts:** macOS, Linux, Windows (see table)

| Host | Status |
|---|---|
| macOS (Apple silicon) | built and run in the emulator; full speed |
| Linux (x86_64) | built by CI (GitHub Actions) on every push; emulator needs KVM |
| Windows | built by CI through Git Bash (experimental); WSL2 also works and is treated as Linux |
| Physical Android device | **not tested** (only emulator) |

## Build the APK

### 1. Install the tools (once)
You need **git, make, a C compiler** (assembles the game), **JDK 17** (not a newer one) and the **Android SDK** with:
`platforms;android-34`, `build-tools;34.0.0`, `cmake;3.22.1`, `ndk;27.3.13750724` (plus `emulator` and a system image to use the emulator).
Easiest: install **Android Studio**, open *Settings > Android SDK > SDK Tools*, tick those. Or from the command line
(get the "command line tools only" zip from https://developer.android.com/studio and unpack it to `<SDK>/cmdline-tools/latest`):

```sh
# macOS:   xcode-select --install; brew install openjdk@17         SDK: ~/Library/Android/sdk
# Linux:   sudo apt install git build-essential openjdk-17-jdk unzip   SDK: ~/Android/Sdk
# Windows: install Git for Windows (gives Git Bash) + JDK 17; the game assembler needs make and gcc, so the
#          simplest route is WSL2 (Ubuntu) and following the Linux steps inside it.
export ANDROID_HOME=$HOME/Android/Sdk          # wherever you put the SDK
yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" "platform-tools" "platforms;android-34" "build-tools;34.0.0" "cmake;3.22.1" "ndk;27.3.13750724"
```

### 2. Get the game and the emulator core
```sh
./setup.sh
```
Downloads (into git-ignored `deps/`) Jeff's source, his `rmac`/`rln` assembler tools, and the Virtual Jaguar core (with
`patches/virtualjaguar-blitter.patch` applied), assembles the game, verifies it against the 1994 build and copies it into the app's assets. About 20 s.

### 3. Build
```sh
./build.sh              # -> Tempest2000-debug.apk
./build.sh release      # -> Tempest2000-release.apk (signed with the debug key; use your own keystore to publish)
```
First build downloads Gradle and the Android plugin. `build.sh` looks for the JDK and SDK in the usual places; set `JAVA_HOME` / `ANDROID_HOME` otherwise.
Or open this folder in Android Studio (after `./setup.sh`) and press Run.

### 4. Run it
Phone/tablet (USB debugging on): `adb install -r Tempest2000-debug.apk`, or copy the APK over and open it.

Emulator on your computer:
```sh
./emulator.sh              # creates a virtual Pixel 6 on first use, installs the APK, starts the game
./emulator.sh --headless   # no window
./emulator.sh --stop
```
It picks the arm64 image on Apple silicon / ARM Linux and the x86_64 image on Intel/AMD. Linux needs `/dev/kvm` (enable virtualisation, add yourself to the `kvm` group).
The x86_64 build is compile-verified only; it has not been run.

## Controls
| Action | Touch | Keyboard | Controller |
|---|---|---|---|
| Move / menus | d-pad | arrows or WASD | d-pad / left stick |
| Fire | FIRE | Space or X | A / Cross |
| Jump | JUMP | Z | B / Circle |
| Superzapper | ZAP | C or V | X, Y, shoulders, triggers |
| Option | OPTION | Enter or O | Start |
| Pause | PAUSE | P or Backspace | Select |

## How it works
[`t2k_android.c`](app/src/main/cpp/t2k_android.c) hosts the core: one emulated frame per screen refresh via `Choreographer`
(time-based, so 60/90/120 Hz screens all get 60 game frames per second); AAudio output with a lock-free queue, one-sample drift
correction and an adaptive buffer; picture drawn on its own thread so a slow display never stalls the game; the core's fast blitter and
idle-loop skip switched on; every press lasts at least 3 frames. The core is built for NEON (arm64) or SSE2 (x86_64) blitting.
`MainActivity` / `ControlsView` handle the vsync loop, input and touch overlay. Measured in the arm64 emulator on an Apple M4:
2 - 3.3 ms emulation per 16.7 ms frame, 0 audio underruns after start-up, 139 MB memory.

## Troubleshooting
| Problem | Fix |
|---|---|
| `Emulator core not found ... Run ./setup.sh` | run `./setup.sh` |
| "game data (t2000.abs) is missing" in the app | `./setup.sh`, then `./build.sh` |
| Gradle "Unsupported class file major version" | use JDK 17 |
| `SDK location not found` / NDK errors | install the SDK packages (step 1), set `ANDROID_HOME` |
| setup.sh can't reach `tiddly.mooo.com` (rln linker source) | that server is occasionally down; retry later |
| Emulator slow (Linux) | needs KVM |

## Credits
Tempest 2000 by Jeff Minter / Llamasoft (source: [mwenge/tempest2k](https://github.com/mwenge/tempest2k)); emulator core
[libretro/virtualjaguar-libretro](https://github.com/libretro/virtualjaguar-libretro); assembler tools rmac/rln. This repo's own code is the Android host and scripts.
