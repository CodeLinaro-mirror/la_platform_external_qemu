# Tool to generate QEMU bazel build files

## Overview

This an internal tool that allows you to generate the bazel backend on Google buildbots
and merge the final results back together. It will string any build information from the
headers and replace it with the generic "rel_dir" prefix.

### Audience

This document is intended for developers and testers working with the Android emulator.

### Prerequisites

* oauth2l: Ensure oauthl2 (or ~/go/bin/oauth2l) is installed. If not, obtain it from [go/oauth2l](http://go/oauth2l).

## Quick start

1. Generate a cl to trigger a presubmit

   Usually you just have created a change to the meson build configuration and need a new bazel build.
   The build system should detect changes in the QEMU branch, and during presubmit will create the build files
   for you.

2. Run the tool:

          fetch-bazel.sh --cl <change-cl>

     This will automatically query Gerrit/TreeHugger for the presubmit build IDs (`emulator_linux_x64:git_emu-main-next`, `emulator_windows_x64:git_emu-main-next`, `emulator_mac_aarch64:git_emu-main-next`), download and merge the generated Bazel files, create a git commit, and send it out for review to Gerrit. You can also explicitly override any build ID using `--win-bid`, `--lin-bid`, or `--mac-bid`.

3. Dry run (optional):

          fetch-bazel.sh --cl <change-cl> --dry-run

     This will execute all the steps, apart from any git (or repo) operations.

## Detailed usage

### Install

You can permanently install fetch-bazel by running `pip install`. This will install the fetch-bazel command. You can get usage instructions
by running:

     fetch-bazel -h

## Known Issues

* Your token has expired. Simply run `~/go/bin/oauth2l reset`
* Make sure to use build ids (`--win-bid xxx`, etc) for which all targets are green.
