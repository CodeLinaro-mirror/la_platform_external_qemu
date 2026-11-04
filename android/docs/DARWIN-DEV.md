# Android Emulator macOS Development

This guide will help you set up and build the Android Emulator on your Mac
(`x86_64` or Apple Silicon `arm64`) for development.

## What You Need

- **Xcode & macOS SDK:**
  - The build system requires a supported macOS SDK version: **`13.3`**,
    **`15.2`**, **`15.5`**, **`26.0`**, **`26.2`**, or **`26.4`** (enforced by
    [`gen-android-sdk-toolchain.sh`](../scripts/unix/gen-android-sdk-toolchain.sh)).
  - Our CI builders use **Xcode 14.3** (macOS SDK 13.3) or newer Xcode releases
    that bundle one of the supported SDKs above.
- **Rosetta 2 (Apple Silicon `arm64` Macs):**
  - Required because several prebuilt host tools in `prebuilts/` (`cmake`,
    `ninja`, `python`) are `x86_64` binaries.
- **Homebrew Packages:**
  - `xz`, `md5sha1sum`, `openjdk@11`, and `python@3.12`.

## Setting Up Your Mac

1. **Install Xcode & Command Line Tools:**

   - Install Xcode into `/Applications/Xcode.app`.
   - Accept the license, select Xcode, and install additional component
     packages:

     ```bash
     sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
     sudo xcodebuild -license accept
     sudo xcodebuild -runFirstLaunch
     for pkg in /Applications/Xcode.app/Contents/Resources/Packages/*.pkg; do
         sudo installer -pkg "$pkg" -target /
     done
     ```

   - Verify your installed Xcode and macOS SDK versions:

     ```bash
     xcodebuild -version
     xcodebuild -showsdks
     ```

   - **Note for Googlers:** If your installed Xcode does not bundle one of the
     supported macOS SDKs (`13.3`, `15.2`, `15.5`, `26.0`, `26.2`, `26.4`), you
     can fetch macOS SDK 13.3 into your Xcode installation by running:

     ```bash
     ./android/scripts/unix/get-osx-sdk.sh --sdk-version 13.3
     ```

2. **Install Rosetta 2 (Apple Silicon Macs only):**

   ```bash
   softwareupdate --install-rosetta --agree-to-license
   ```

3. **Install Homebrew Dependencies:**

   ```bash
   brew install xz md5sha1sum openjdk@11 python@3.12
   sudo ln -sfn "$(brew --prefix openjdk@11)/libexec/openjdk.jdk" /Library/Java/JavaVirtualMachines/openjdk-11.jdk
   ```

   _(Optional)_ If you plan to rebuild third-party prebuilts (such as Qt or
   Mesa), also install:

   ```bash
   brew install node@20 gperf bison@3.8 flex
   python3 -m pip install --break-system-packages html5lib Mako packaging PyYAML
   ```

## Getting the Source Code

1. **Install Repo:**

   ```bash
   mkdir -p $HOME/bin
   curl https://storage.googleapis.com/git-repo-downloads/repo > $HOME/bin/repo
   chmod 700 $HOME/bin/repo
   export PATH=$PATH:$HOME/bin
   ```

   Add `export PATH=$PATH:$HOME/bin` to your `.zshrc` or `.bashrc` file.

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
   `objs/distribution/emulator/`.

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

  Alternatively, run the `Compile` task directly via `rebuild.sh`:

  ```bash
  ./android/rebuild.sh --task Compile
  ```

- **Configuring the Build:**

  You can inspect all available options and build tasks by running:

  ```bash
  ./android/rebuild.sh --help
  ```

  For example, `./android/rebuild.sh --task CTest` runs the unit test suite, and
  `./android/rebuild.sh --config debug` builds a debug configuration.

- **Running Unit Tests:**

  The recommended way to run unit tests is via the `CTest` task, which sets up
  the required environment variables:

  ```bash
  ./android/rebuild.sh --task CTest
  ```

  You can also invoke the prebuilt `ctest` directly in `objs`:

  ```bash
  cd objs && ../../../prebuilts/cmake/darwin-x86/bin/ctest -j 10 --output-on-failure
  ```

### Downloading Pre-built Emulators

You can obtain the latest build of the emulator from our
[build servers](https://ci.android.com/builds/branches/aosp-emu-main-dev/grid?legacy=1).
Look for the `sdk-repo-{architecture}-emulator-{buildid}.zip` artifact.

You might need to remove a quarantine flag before running them:

```bash
xattr -dr com.apple.quarantine /path/to/my/unzipped/emulator
```

### Sending patches

Here you can find details on
[submitting patches](https://source.android.com/docs/setup/contribute/submit-patches).
