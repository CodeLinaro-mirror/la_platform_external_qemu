# Android Emulator Linux Development

This guide will help you set up and build the Android Emulator on your Linux
machine for development.

## What You Need

The instructions here assume a Debian- or Ubuntu-based distribution (such as
Ubuntu 24.04 LTS or Debian/gLinux).

1. **Install Required Build and Runtime Dependencies:**

   ```bash
   sudo apt-get update && sudo apt-get install -y \
       git build-essential g++ autoconf automake libtool libtool-bin pkg-config \
       unzip zip curl ca-certificates gnupg \
       python3 python-is-python3 python3-venv python3-pip \
       libxml2-utils libx11-dev libgl-dev libx11-xcb-dev libxkbcommon-dev \
       libwayland-dev libxrandr-dev \
       libpulse0 libegl-mesa0 libglu1-mesa libxcb-keysyms1 \
       openjdk-21-jre-headless qemu-kvm
   ```

   - **Ubuntu 24.04+ Note (`libtinfo5`):** Prebuilt Clang/LLVM toolchain
     binaries may require `libtinfo.so.5`. On Ubuntu 24.04 (which ships
     `libtinfo6` by default), install `libtinfo5` if it is not already present:
     ```bash
     curl -LO http://security.ubuntu.com/ubuntu/pool/universe/n/ncurses/libtinfo5_6.3-2ubuntu0.1_amd64.deb
     sudo dpkg -i libtinfo5_6.3-2ubuntu0.1_amd64.deb
     rm libtinfo5_6.3-2ubuntu0.1_amd64.deb
     ```
   - **Cross-compiling for `linux_aarch64` (Optional):** If you plan to
     cross-compile for `linux_aarch64`
     (`./android/rebuild.sh --target linux_aarch64`), install the AArch64
     cross-compilers:
     ```bash
     sudo apt-get install -y g++-aarch64-linux-gnu
     ```
   - **Rebuilding Vulkan Loader Prebuilts (Optional):** If you need to rebuild
     the `vulkan_loader` prebuilt (`--prebuilts vulkan_loader`), install
     `podman`:
     ```bash
     sudo apt-get install -y podman
     ```

2. **Configure KVM Permissions:**

   Ensure your user belongs to the `kvm` group so hardware acceleration works:

   ```bash
   sudo gpasswd -a $USER kvm
   ```

   _(Log out and back in, or run `newgrp kvm`, for group membership changes to
   take effect.)_

> **Note:** Prebuilt versions of `cmake`, `ninja`, `python3`, `clang`, `rust`,
> and `sccache` are included in the repository checkout under
> `$HOME/emu-main-dev/prebuilts/` and are used automatically by
> `android/rebuild.sh`.

## Getting the Source Code

1. **Install Repo:**

   ```bash
   mkdir -p $HOME/bin
   curl https://storage.googleapis.com/git-repo-downloads/repo > $HOME/bin/repo
   chmod 700 $HOME/bin/repo
   export PATH=$PATH:$HOME/bin
   ```

   Add `export PATH=$PATH:$HOME/bin` to your `.bashrc` or `.zshrc` file.

2. **Initialize Repo:**

   ```bash
   mkdir -p $HOME/emu-main-dev && cd $HOME/emu-main-dev
   repo init -u https://android.googlesource.com/platform/manifest -b emu-main-dev
   ```

3. **Download Code:**

   ```bash
   repo sync -j 8
   ```

## Building the Emulator

1. **Build:**

   ```bash
   cd external/qemu && ./android/rebuild.sh
   ```

   This configures the CMake build in `objs/`, compiles the emulator, runs unit
   tests, and installs the runnable distribution into
   `objs/distribution/emulator/` (as well as unstripped binaries into
   `objs/distribution-unstripped/emulator/`).

2. **List Available Virtual Devices:**

   ```bash
   ./objs/distribution/emulator/emulator -list-avds
   ```

## Additional Notes

- **Setting up the Shell Environment & Incremental Builds:**

  Source `android/envsetup.sh` to add the prebuilt `ninja` to your `PATH` and
  configure `ASAN_OPTIONS`:

  ```bash
  source android/envsetup.sh
  ninja -C objs install
  ```

  Alternatively, you can run incremental builds directly through `rebuild.sh`:

  ```bash
  ./android/rebuild.sh --task Compile
  ```

- **Configuring the Build:**

  You can inspect all available options and build tasks by running:

  ```bash
  ./android/rebuild.sh --help
  ```

  Common options include:

  - `--config debug`: Build a debug configuration.
  - `--sanitizer address`: Build with AddressSanitizer enabled (`address` or
    `thread`).
  - `--target linux_aarch64`: Cross-compile for Linux AArch64.
  - `--task <TaskName>`: Run a specific task (`Configure`, `Compile`, `CTest`,
    `Distribution`, `Clean`, `RemoveLeftover`). Use `--task-list` to see all
    tasks.

- **Running Unit Tests:**

  The recommended way to run unit tests is via the `CTest` task, which
  automatically configures the required SwiftShader/ANGLE/Vulkan environment
  variables:

  ```bash
  ./android/rebuild.sh --task CTest
  ```

  You can also run `ctest` directly from the `objs` directory using the prebuilt
  CMake toolchain:

  ```bash
  cd objs && ../../../prebuilts/cmake/linux-x86/bin/ctest -j 10 --output-on-failure
  ```

### Downloading Pre-built Emulators

You can obtain the latest build of the emulator from our
[build servers](https://ci.android.com/builds/branches/aosp-emu-main-dev/grid?legacy=1).
Look for the `sdk-repo-{architecture}-emulator-{buildid}.zip` artifact.

### Sending patches

Here you can find details on
[submitting patches](https://source.android.com/docs/setup/contribute/submit-patches).
