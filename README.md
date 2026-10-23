# Welcome to the Android Emulator

The Android Emulator allows you to test your Android apps on a variety of
virtual devices and API levels without needing physical devices. It simulates
almost all features of a real Android device, including phone calls, text
messages, location, network speeds, sensors, and access to the Google Play
Store.

## Getting Started

- Install [Android Studio](https://developer.android.com/studio) and the
  required SDKs.
- Install the official emulator and create Android Virtual Devices (AVDs).
- Follow platform-specific instructions to set up your environment and build the
  emulator on Linux, macOS, or Windows:

  - [On Linux](android/docs/LINUX-DEV.md)
  - [On macOS](android/docs/DARWIN-DEV.md)
  - [On Windows](android/docs/WINDOWS-DEV.md)

## Quick Build & Test Reference

Once your platform prerequisites and `repo` checkout (`emu-main-dev`) are set
up:

- **Full Build (Configure, Compile, Test, & Package):**
  - Linux / macOS: `cd external/qemu && ./android/rebuild.sh`
  - Windows: `cd external\qemu && android\rebuild`
- **Runnable Emulator Distribution:**
  - The packaged emulator binary and all runtime dependencies (`lib64/`, Qt
    plugins, SwiftShader/ANGLE, `advancedFeatures.ini`, etc.) are installed
    under `objs/distribution/emulator/emulator` (and unstripped binaries on
    Linux under `objs/distribution-unstripped/emulator/emulator`).
- **Incremental Build:**
  - `./android/rebuild.sh --task Compile` (or
    `source android/envsetup.sh && ninja -C objs install`)
- **Run Unit Tests:**
  - `./android/rebuild.sh --task CTest`

## Developer Documentation

- [Development & Architecture Guide](android/docs/DEVELOPMENT.md)
- [Coding Style Guide](android/docs/ANDROID-CODING-STYLE.md)
- [Build System Overview](android/build/README)
- [Submitting Patches](https://source.android.com/docs/setup/contribute/submit-patches)
