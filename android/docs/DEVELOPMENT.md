# Android Emulator Development & Architecture Guide

This document provides an overview of the Android Emulator codebase, build
pipeline, directory structure, and runtime layout for contributors.

---

## I. Getting the Sources

For full platform-specific setup instructions and prerequisites, see:

- [Linux Development Guide](LINUX-DEV.md)
- [macOS Development Guide](DARWIN-DEV.md)
- [Windows Development Guide](WINDOWS-DEV.md)

### Checkout with `repo`

Active emulator development happens on the `emu-main-dev` branch:

```bash
mkdir emu-main-dev && cd emu-main-dev
repo init -u https://android.googlesource.com/platform/manifest -b emu-main-dev
repo sync -j 8
```

---

## II. Building the Emulator

### II.1) Build Pipeline Overview (`android/rebuild.sh` / `cmake.py`)

The emulator uses a CMake + Ninja build system orchestrated by a Python task
runner located at
[`android/build/python/aemu/cmake.py`](../build/python/aemu/cmake.py).

- On Linux and macOS, [`./android/rebuild.sh`](../rebuild.sh) invokes
  `$AOSP/prebuilts/python/<host>-x86/bin/python3 android/build/python/cmake.py`.
- On Windows, [`android\rebuild.cmd`](../rebuild.cmd) invokes
  `python android\build\python\cmake.py` and automatically locates Visual Studio
  2022 via `vswhere.exe`.

To perform a full build (configure, compile, unit tests, and distribution
packaging):

```bash
cd $TOP/external/qemu
./android/rebuild.sh
```

By default, build artifacts are placed under `objs/` (configurable with
`--out <path>`), and the runnable emulator distribution is assembled in:

```text
objs/distribution/emulator/
```

On Linux, an unstripped distribution is also generated in
`objs/distribution-unstripped/emulator/`.

### II.2) Build Tasks and Options

Run `./android/rebuild.sh --help` to see all options. Key flags include:

- `--task <TaskName>`: Execute a specific build task instead of the full
  pipeline (use `--task-list` to list all tasks, or `--task-enable` /
  `--task-disable`):
  - `Clean`: Remove the output directory (`objs/`).
  - `RemoveLeftover`: Terminate any lingering Windows build/test processes
    running from `objs/`.
  - `Prebuilts`: Build or verify third-party prebuilts.
  - `Configure`: Generate Ninja build files in `objs/` using CMake and the
    target toolchain.
  - `Compile`: Build all targets and install into `objs/distribution/emulator/`
    (plus `objs/distribution-fishtank/` and, on Linux,
    `objs/distribution-unstripped/emulator/`).
  - `CTest`: Run the GoogleTest/CTest unit test suite with required
    graphics/Vulkan environment variables.
  - `Distribution`: Create redistributable
    `sdk-repo-<host>-emulator-<build_id>.zip` archives (enabled when `--dist` is
    specified).
  - `IntegrationTest`: Run integration tests.
- `--target <platform>`: Target platform (`linux`, `linux_aarch64`, `darwin`,
  `darwin_aarch64`, `windows`).
- `--config <debug|release>`: Select debug (`-O0 -g3`) or release (`-O3 -g3`
  with symbols extracted) configuration.
- `--sanitizer <address|thread>`: Enable Clang sanitizers on supported targets.
- `--crash <prod|staging|none>`: Enable Breakpad/Crashpad symbol generation and
  crash reporting.
- `--ccache <auto|none|/path/to/cache>`: Configure compiler caching. By default
  (`auto`), the build system uses the prebuilt `sccache` binary under
  `$TOP/prebuilts/android-emulator-build/common/sccache/`.
- `--feature <feature>` / `--feature no-<feature>`: Enable or disable optional
  CMake features (e.g., `--feature no-rust`, `--feature webengine`; use
  `--feature-list` to list all available features).

### II.3) Incremental Builds

After the initial configuration, you can recompile and update
`objs/distribution/emulator/` incrementally using either:

```bash
./android/rebuild.sh --task Compile
```

or (on Linux/macOS after sourcing `android/envsetup.sh`):

```bash
source android/envsetup.sh
ninja -C objs install
```

---

## III. Testing

### III.1) Unit Tests

Most unit tests are written in C++ using GoogleTest and registered with CTest.
Always run unit tests via the `CTest` task or with the proper
Vulkan/SwiftShader/ANGLE environment variables:

```bash
./android/rebuild.sh --task CTest
```

### III.2) Booting an Existing AVD

To test your locally built emulator against an existing Android Studio AVD:

```bash
export ANDROID_SDK_ROOT=/path/to/installed/android/sdk
./objs/distribution/emulator/emulator -avd <avd_name>
```

### III.3) Booting a Local AOSP Platform Build

If you have built an emulator-compatible AOSP target (such as
`sdk_phone64_x86_64-userdebug` or `sdk_phone64_arm64-userdebug`):

```bash
export ANDROID_BUILD_TOP=/path/to/aosp
export ANDROID_PRODUCT_OUT=/path/to/aosp/out/target/product/emu64x
./objs/distribution/emulator/emulator
```

---

## IV. Source Tree Layout

The Android Emulator combines an upstream-derived QEMU2 core
(`$TOP/external/qemu/`) with Android-specific libraries, UI, virtual device
glue, and host graphics stacks across the `emu-main-dev` manifest:

### IV.1) Under `$TOP/external/qemu/`

- **Root QEMU2 directories (`accel/`, `bsd-user/`, `chardev/`, `cpus.c`, `hw/`,
  `include/`, `migration/`, `net/`, `softmmu/`, `target/`, `tcg/`, `ui/`,
  `util/`, etc.):**
  - The QEMU2 emulation engine, supporting the `ranchu` (virtio-based) virtual
    platform across `x86_64` and `aarch64` targets, with hardware hypervisor
    backends (`KVM` on Linux, `HVF` on macOS, `WHPX` and `AEHD` on Windows).
- **`android-qemu2-glue/`:**
  - Glue layer connecting the QEMU2 engine to the `android-emu` libraries,
    virtual device agents, looper, snapshots, and Goldfish/Ranchu devices.
- **`android/android-emu-base/` (`aemu/base/`):**
  - Foundational C++ platform abstractions (threading, async loopers, sockets,
    streams, shared memory, system mocks, logging, strings, containers).
- **`android/emu/`:**
  - Modularized emulator components (`agents/`, `avd/`, `base/`, `check/`,
    `cmdline/`, `crashreport/`, `curl/`, `daemon/`, `feature/`, `files/`,
    `gps/`, `hardware/`, `metrics/`, `proxy/`, `sockets/`, `studio-config/`,
    `telephony/`, `utils/`, etc.).
- **`android/android-emu/`:**
  - High-level Android emulation features (snapshot management, ADB interface,
    camera, sensors, screen recording, virtual scene, foldable/posture
    simulation, console, etc.).
- **`android/android-ui/`:**
  - The Qt6-based graphical user interface, skin engine, extended controls
    window, and toolbars.
- **`android/android-emugl/`:**
  - Host GPU emulation orchestration (`libOpenglRender`, render threads,
    framebuffers, translator libraries, and integration with `gfxstream`,
    `ANGLE`, and `SwiftShader`).
- **`android/android-grpc/`:**
  - gRPC controller service (`EmulatorController`, snapshot service, UI
    controller, vehicle/car services, waterfall/adb forwarding) for programmatic
    control and Android Studio embedded emulator streaming.
- **`android/android-webrtc/`:**
  - WebRTC video/audio/input streaming bridge.
- **`android/android-net/` & `android/bluetooth/`:**
  - Networking, Wi-Fi, packet capture, and Bluetooth/netsim integration.
- **`android/emulator/`:**
  - Source for the lightweight `emulator` launcher binary.
- **`android/emulator-check/`:**
  - Source for the `emulator-check` utility used by Android Studio to query host
    hypervisor/CPU capabilities.
- **`android/third_party/`:**
  - CMake build definitions integrating third-party dependencies (`libpng`,
    `zlib`, `protobuf`, `grpc`, `ffmpeg`, `libyuv`, `breakpad`, `crashpad`,
    `astc-codec`, etc.).

### IV.2) Related Repositories in the `emu-main-dev` Manifest

- **`$TOP/hardware/google/gfxstream/`:**
  - Modern graphics streaming host and guest libraries (`gfxstream`, Virtio-GPU
    Vulkan/GLES/Composer host renderer).
- **`$TOP/hardware/google/aemu/`:**
  - Shared `aemu` base, host-common, and snapshot libraries used by both the
    emulator and `gfxstream`.
- **`$TOP/external/crosvm/rutabaga_gfx/`:**
  - Rutabaga virtualized graphics interface (Rust/C++).
- **`$TOP/tools/netsim/`:**
  - `netsimd` wireless network simulator (Bluetooth, Wi-Fi, UWB, NFC) built via
    Cargo/CMake and bundled with the emulator distribution.

---

## V. Overview of Installed Binaries (`objs/distribution/emulator/`)

A built emulator distribution under `$EXEC_DIR` (`objs/distribution/emulator/`)
contains:

- **`$EXEC_DIR/emulator`:**
  - The main entry point launcher. It parses command-line flags and AVD config
    files, configures `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` / `PATH` for GPU
    and Qt libraries, and launches the appropriate `qemu-system-<arch>` engine.
- **`$EXEC_DIR/qemu/<host-system>/qemu-system-<arch>`:**
  - The QEMU2 emulation engines (`qemu-system-x86_64`, `qemu-system-aarch64`,
    and headless variants `qemu-system-<arch>-headless`).
- **`$EXEC_DIR/lib64/`:**
  - Shared libraries required at runtime, including:
    - `libOpenglRender`, `libgfxstream_backend`, `libEGL_translator`,
      `libGLES_CM_translator`, `libGLES_V2_translator`, `libshadertranslator`
    - `qt/` (`lib/` and `plugins/` for the Qt6 UI)
    - `gles_swiftshader/`, `gles_angle/`, and `vulkan/` (ICD drivers,
      `libvulkan`, MoltenVK on macOS, Lavapipe/SwiftShader software renderers)
- **`$EXEC_DIR/emulator-check`:**
  - Host capability query tool called by Android Studio (e.g.,
    `emulator-check accel`).
- **`$EXEC_DIR/netsimd` & `$EXEC_DIR/netsim`:**
  - Multi-device RF/wireless simulation daemon and CLI.

---

## VI. Prebuilt Dependencies

Prebuilt toolchains and libraries live under `$TOP/prebuilts/`:

- **`$TOP/prebuilts/clang/host/<os>-x86/`:** Clang/LLVM C/C++ compiler toolchain
  (configured in [`android/build/toolchains.json`](../build/toolchains.json)).
- **`$TOP/prebuilts/rust/<os>-x86/`:** Rust compiler and Cargo toolchain.
- **`$TOP/prebuilts/cmake/<os>-x86/` & `$TOP/prebuilts/ninja/<os>-x86/`:**
  CMake, CTest, and Ninja build tools.
- **`$TOP/prebuilts/python/<os>-x86/`:** Python 3 runtime used on Linux and
  macOS.
- **`$TOP/prebuilts/android-emulator-build/`:** Prebuilt third-party libraries
  (`qt`, `angle`, `curl`, `ffmpeg`, `grpc`, `protobuf`, `qemu-android-deps`,
  `sccache`, `vulkan`, etc.).

Several third-party prebuilts can be rebuilt directly through `rebuild.sh` using
the modules in
[`android/build/python/aemu/prebuilts/`](../build/python/aemu/prebuilts/):

```bash
./android/rebuild.sh --prebuilts <qt|angle|vulkan_loader>
```
