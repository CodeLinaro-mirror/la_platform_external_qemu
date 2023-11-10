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

          fetch-bazel.sh --cl <change-cl> --win-bid <windows-presubmit> --lin-bid <linux-presubmit> --mac-bid <mac-presubmit>

     This will create a git commit and send it out for review to gerrit.

3. Dry run (optional):

          fetch-bazel.sh --cl <change-cl> --win-bid <windows-presubmit> --lin-bid <linux-presubmit> --mac-bid <mac-presubmit> --dry-run

     This will execute all the steps, apart from any git (or repo) operations.

## Detailed usage

### Install

You can permanently install fetch-bazel by running `pip install`. This will install the fetch-bazel command. You can get usage instructions
by running:

     fetch-bazel -h

## Known Issues

* Your token has expired. Simply run `~/go/bin/oauth2l reset`
* Make sure to use build ids (`--win-bid xxx`, etc) for which all targets are green.
