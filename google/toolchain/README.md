# QEMU to Bazel Translation Workflow

This document explains how the AEMU project translates QEMU's native Meson build system into Bazel `BUILD` files. It is intended for engineers who need to modify build configurations, add new targets, or understand the build pipeline.

## Overview

QEMU upstream uses Meson as its primary build system. To integrate QEMU into the AOSP Bazel ecosystem, we use a custom tool called `amc` (Android Meson Configurator) to drive Meson and generate Bazel files, which are later merged into a single, unified `BUILD.bazel` file.

## Key Files

### 1. `qemu-build-config.jsonc`
Located at `third_party/qemu/google/toolchain/qemu-build-config.jsonc`, this is the main configuration file for the `amc` tool. It defines:
*   **Dependencies**: Mapping of package names to Bazel targets.
*   **Meson Options**: Feature flags passed to `meson setup` (e.g., `-Dvirtfs=disabled`).
*   **Platforms**: Platform-specific overrides for dependencies and Meson options.

### 2. `BUILD.common`
Located at `third_party/qemu/google/build/BUILD.common`, this file contains standard Bazel rules and variables that are merged **verbatim** into the final `BUILD.bazel` file. This is the place to define shared variables or custom rules that the generated rules might need.

### 3. Shims in `qemu-build-config.jsonc`
Located in `third_party/qemu/google/toolchain/qemu-build-config.jsonc`, the shims (rule overrides) are now integrated directly into the main configuration file under the `common` and platform-specific sections in `platforms`. They allow fine-grained modification of the generated Bazel rules using regex matching on target names. They can add/remove sources, dependencies, or modify command lines.

## The Translation Flow

The process of going from Meson to the final `BUILD.bazel` file involves the following steps:

1.  **Generation**:
    *   The `amc bazel` command is run for each target platform.
    *   It drives Meson with `--backend bazel` and passes the options from `qemu-build-config.jsonc`.
    *   Meson generates a platform-specific Bazel file (e.g., `BUILD.linux-x86_64`).
    *   This step is typically performed on buildbots during presubmit.

2.  **Fetching**:
    *   Developers use the `fetch-bazel` tool (see `google/scripts/fetch_bazel/README.md`) to pull down these generated platform-specific files from the buildbots.

3.  **Merging**:
    *   The `merge_bazel.py` script is invoked.
    *   It reads all the platform-specific files and merges them.
    *   If a rule is identical across platforms, it is emitted once.
    *   If attributes differ, it uses Bazel `select()` statements to handle the differences.
    *   It also reads `BUILD.common` and appends its content verbatim.
    *   The result is the monolithic `third_party/qemu/BUILD.bazel`.

## How-To Guides

### Changing QEMU Feature Flags
To enable or disable a QEMU feature:
1.  Open `third_party/qemu/google/toolchain/qemu-build-config.jsonc`.
2.  Locate the `meson_options` section.
3.  Modify the desired flag (e.g., change `"-Dvnc": "disabled"` to `"enabled"`).
4.  If the change is platform-specific, make the modification under the appropriate platform in the `platforms` section.
5.  Submit a CL. The buildbots will generate new Bazel files.
6.  Run `fetch-bazel` to pull the new files and update `BUILD.bazel`.

### Introducing a New Target (e.g., `linux-arm64` or `windows-arm64`)
To add support for a new host architecture:
1.  Open `qemu-build-config.jsonc`.
2.  Add a new entry under the `platforms` dictionary (e.g., `"linux-aarch64"`).
3.  Define any platform-specific dependencies and `meson_options` for this new platform.
4. If needed, add a `"shims"` array under your new platform in `qemu-build-config.jsonc` to handle rule overrides.
5.  Update the buildbot configuration to run `amc bazel` for this new target.
6.  Update the invocation of `merge_bazel.py` (often in `fetch-bazel` or a wrapper script) to include the new generated file in the merge process.
