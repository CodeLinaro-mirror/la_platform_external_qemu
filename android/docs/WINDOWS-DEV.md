# Android Emulator Windows Development

This guide helps you set up the Android Emulator on Windows for development. You
can use your own computer or a Google Compute Engine (GCE) workstation.

## Software Requirements

- **Visual Studio 2022:** Community, Professional, or Enterprise edition with
  the **Desktop development with C++** workload
  (`Microsoft.VisualStudio.Workload.NativeDesktop`) and the following
  components:
  - MSVC v143 C++ x64/x86 build tools
    (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`)
  - Windows 11 SDK (`Microsoft.VisualStudio.Component.Windows11SDK.22621`)
  - C++ ATL and MFC support (`Microsoft.VisualStudio.Component.VC.ATL`,
    `Microsoft.VisualStudio.Component.VC.ATLMFC`) — required for `atlbase.h` and
    `atls.lib`
  - C++ CMake tools for Windows
    (`Microsoft.VisualStudio.Component.VC.CMake.Project`)
  - C++ Profiling tools / DIA SDK (`dia2.h` and `diaguids.lib`, included with
    Desktop development with C++)
- **Windows Assessment and Deployment Kit (`windows-adk`):** Required for
  `dismapi.h` and `dismapi.lib` when building `emulator-check`.
- **Python 3 (3.11+):** Installed and added to your system `PATH`, along with
  the required Python packages (`psutil`, `packaging`, `Mako`, `PyYAML`,
  `configparser`, `certifi`).
- **OpenJDK (`microsoft-openjdk`):** Added to `PATH`.
- **Git & Curl:** Added to `PATH`.

### Installing Dependencies with Chocolatey

From an **elevated PowerShell** prompt, you can install Chocolatey and all
required build dependencies using the exact configuration provisioned on our
Windows build machines:

```powershell
# 1. Install Chocolatey (if not already installed)
Set-ExecutionPolicy Bypass -Scope Process -Force
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$choco_url = 'https://community.chocolatey.org/install.ps1'
iex ((New-Object System.Net.WebClient).DownloadString($choco_url))

# 2. Install Git, OpenJDK, Windows ADK, and Python 3 (into C:\python3)
choco install -y git --params "'/GitOnlyOnPath'"
choco install -y curl microsoft-openjdk windows-adk
choco install -y python313 --params "/InstallDir:C:\python3"

# 3. Refresh PATH, create python3.exe symlink, and install required pip packages
$env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
$py_dir = 'C:\python3'
New-Item -ItemType SymbolicLink -Path "$py_dir\python3.exe" -Target "$py_dir\python.exe" -Force
$pip_pkgs = @('certifi', 'pip', 'psutil', 'configparser', 'Mako', 'packaging', 'PyYAML')
& "$py_dir\python3.exe" -m pip install --upgrade $pip_pkgs

# 4. Install Visual Studio 2022 Community with required C++, ATL/MFC, and SDK components
$vs_packages = @(
  '--add Microsoft.VisualStudio.Workload.NativeDesktop',
  '--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
  '--add Microsoft.VisualStudio.Component.Windows11SDK.22621',
  '--add Microsoft.VisualStudio.Component.VC.ATL',
  '--add Microsoft.VisualStudio.Component.VC.ATLMFC',
  '--add Microsoft.VisualStudio.Component.VC.CMake.Project'
) -join ' '
choco install -y visualstudio2022community --package-parameters $vs_packages

# 5. Install BusyBox POSIX utilities (used by build scripts/genrules)
New-Item -ItemType Directory -Path 'C:\busybox' -Force
$busybox_url = 'https://frippery.org/files/busybox/busybox64.exe'
(New-Object System.Net.WebClient).DownloadFile($busybox_url, 'C:\busybox\busybox64.exe')
& C:\busybox\busybox64.exe --install
New-Item -Path 'C:\msys64\usr' -ItemType Directory -Force
New-Item -ItemType SymbolicLink -Path 'C:\msys64\usr\bin' -Target 'C:\busybox' -Force
```

### Enable Developer Mode, Long Paths, & Short Temp Directory

`repo` and the emulator build system create symbolic links and deep file paths.
In an **elevated PowerShell** prompt, enable Windows Developer Mode (which
grants symlink creation privileges without requiring elevated prompts), enable
Win32 Long Paths, configure a short `C:\temp` directory, and optionally exclude
your build directories from Windows Defender real-time scanning (which prevents
antivirus file-locking errors during linking):

```powershell
# Enable Developer Mode (allows creating symlinks without elevation)
$unlock_key = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
& reg add $unlock_key /t REG_DWORD /f /v 'AllowDevelopmentWithoutDevLicense' /d '1'

# Enable Win32 Long Paths
$fs_key = 'HKLM\SYSTEM\CurrentControlSet\Control\FileSystem'
& reg add $fs_key /t REG_DWORD /f /v 'LongPathsEnabled' /d '1'

# Configure a short TEMP/TMP directory to avoid MAX_PATH issues
New-Item -Path "C:\temp" -ItemType Directory -Force
[Environment]::SetEnvironmentVariable('TEMP', 'C:\temp', 'Machine')
[Environment]::SetEnvironmentVariable('TMP', 'C:\temp', 'Machine')
[Environment]::SetEnvironmentVariable('TEMP', 'C:\temp', 'User')
[Environment]::SetEnvironmentVariable('TMP', 'C:\temp', 'User')

# (Recommended) Exclude checkout and temp folders from Windows Defender scanning
Add-MpPreference -ExclusionPath "C:\e", "C:\temp"
```

### Hardware Acceleration

To run x86_64 AVDs on Windows with hardware acceleration, enable either:

- **Android Emulator Hypervisor Driver (AEHD)** (installed via Android Studio
  SDK Manager), or
- **Windows Hypervisor Platform (WHPX)** (`Turn Windows features on or off` ->
  `Windows Hypervisor Platform`).

## Configure Git

Configure Git to preserve line endings, enable symbolic links, support long
paths, and avoid credential manager hangs during `repo sync`:

```bat
git config --system core.autocrlf false
git config --global core.autocrlf false
git config --system core.symlinks true
git config --global core.symlinks true
git config --system core.longpaths true
git config --global core.longpaths true
git config --system credential.helper manager
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
```

## Getting the Source Code

1. **Install Repo:**

   From a Command Prompt (`cmd.exe`), execute:

   ```bat
   mkdir %USERPROFILE%\bin
   cd %USERPROFILE%\bin
   curl -Lo repo https://storage.googleapis.com/git-repo-downloads/repo
   echo @call python %%~dp0repo %%* > %USERPROFILE%\bin\repo.cmd
   ```

   Make sure to add `%USERPROFILE%\bin` to your `PATH`, otherwise you will not
   be able to run `repo`.

2. **Initialize Repo:**

   ```bat
   cd \ && mkdir e && cd e
   repo init -u https://android.googlesource.com/platform/manifest -b emu-main-dev
   ```

   The folder is named `e` (`C:\e`) to keep file paths short and avoid hitting
   `MAX_PATH` limits in legacy tools during the build.

3. **Download Code:**

   ```bat
   repo sync -j 8
   ```

## Building the Emulator

1. **Build:**

   `android\rebuild.cmd` automatically locates your Visual Studio 2022
   installation via `vswhere.exe` and sets up the MSVC environment:

   ```bat
   cd external\qemu && android\rebuild
   ```

   This configures the CMake build in `objs\`, compiles the emulator, runs unit
   tests, and installs the runnable distribution into
   `objs\distribution\emulator\`.

2. **List Available Virtual Devices:**

   ```bat
   objs\distribution\emulator\emulator.exe -list-avds
   ```

## Additional Notes

- **Incremental Builds:**

  ```bat
  android\rebuild --task Compile
  ```

  Or invoke the prebuilt `ninja.exe` directly from an MSVC Developer Command
  Prompt:

  ```bat
  ..\..\prebuilts\ninja\windows-x86\ninja.exe -C objs install
  ```

- **Configuring the Build:**

  You can inspect all available options and build tasks by running:

  ```bat
  android\rebuild --help
  ```

  For example, `android\rebuild --task CTest` runs the unit test suite, and
  `android\rebuild --config debug` builds a debug configuration.

- **Running Unit Tests:**

  The recommended way to run unit tests is via the `CTest` task:

  ```bat
  android\rebuild --task CTest
  ```

  You can also invoke the prebuilt `ctest.exe` directly in the `objs` directory:

  ```bat
  cd objs
  ..\..\..\prebuilts\cmake\windows-x86\bin\ctest.exe -j 10 --output-on-failure
  ```

### Downloading Pre-built Emulators

You can obtain the latest build of the emulator from our
[build servers](https://ci.android.com/builds/branches/aosp-emu-main-dev/grid?legacy=1).
Look for the `sdk-repo-{architecture}-emulator-{buildid}.zip` artifact.

### Sending patches

Here you can find details on
[submitting patches](https://source.android.com/docs/setup/contribute/submit-patches).
