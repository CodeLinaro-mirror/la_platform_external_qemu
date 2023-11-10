# QEMU with Android Extensions

This repository contains a modified version of QEMU, tailored for Android emulation.

## Core Philosophy: Minimal Customization

Compared to previous generations of the Android Emulator, the major difference in this version is the use of **QEMU's module interface** to load plugins and drivers at runtime.

Instead of adding deep, invasive customizations directly to the QEMU codebase, we implement Android-specific functionality as separate components. This architecture:
*   **Minimizes changes on QEMU proper**, making it much easier to merge updates from upstream QEMU.
*   Keeps Android-specific drivers isolated.

## Contents

- **Base QEMU:** The core QEMU functionality with minimal modifications.
- **Android-specific drivers:** Drivers for various hardware components (graphics, sound, networking) needed to run Android. These reside in the `hardware/generic/goldfish` repository and are loaded as plugins.
- **Platform-specific modifications:** Changes to ensure successful compilation on supported platforms:
  - Linux x64
  - macOS (Intel and Apple Silicon)
  - Windows x64

## Building the Android Emulator

The Android Emulator is built using **Bazel**. The build commands should be run from the root of the AOSP workspace (where the `.repo` directory resides).

To build the emulator release package:

```shell
bazel build @goldfish//emulator:release
```

To run the unit tests:

```shell
bazel test @goldfish//emulator/...
```

## Running a Development Build

You can run the emulator directly from within Bazel for development and testing.

### Running the `goldfish` binary

To run the emulator, you can invoke the `goldfish` binary directly via Bazel. You will need to provide your own Android Virtual Device (AVD) name:

```sh
bazel run @goldfish//emulator/launcher -- -avd MyAVD
```

This will launch the goldfish emulator with the specified AVD. You can pass any additional QEMU flags after the `--`.

## Building Stock QEMU with AOSP Toolchain

If you wish to build a standalone version of QEMU using the hermetic AOSP compiler toolchain, you can use the `amc` (Android Meson Configurator) tool provided in the repository.

See the documentation in `google/toolchain/README.md` for detailed instructions on how the translation from Meson to Bazel works and how to use `amc`.

## Contributing

We welcome contributions to improve the Android Emulator.

### Upstream First
When making changes that are not specific to this Android fork, please consider upstreaming them to the official QEMU repository. This reduces maintenance costs and benefits the wider community.

### Android-Specific Changes
For changes that are specific to Android emulation and cannot be upstreamed, please follow the standard Android Open Source Project (AOSP) contribution guidelines using Gerrit for code review.
