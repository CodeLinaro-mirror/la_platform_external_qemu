#!/usr/bin/env python
#
# Copyright 2024 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path
import platform

import aemu.prebuilts.deps.common as deps_common

AOSP_ROOT = Path(__file__).resolve().parents[7]
HOST_OS = platform.system().lower()
HOST_ARCH = platform.machine().lower()

VULKAN_LOADER_REPO_URL = "https://github.com/KhronosGroup/Vulkan-Loader.git"
VULKAN_LOADER_GIT_SHA = "0a278cc725089cb67bf6027076e5d72f97c04d86"
CMAKE_PATH = os.path.join(AOSP_ROOT, "prebuilts", "cmake", HOST_OS + "-x86", "bin")
VULKAN_LOADER_PREBUILTS_ARCH = "x86_64" if HOST_ARCH == "x86_64" else "aarch64"
VULKAN_LOADER_PREBUILTS_PATH = (
    AOSP_ROOT
    / "prebuilts"
    / "android-emulator-build"
    / "common"
    / "vulkan"
    / f"{HOST_OS}-{VULKAN_LOADER_PREBUILTS_ARCH}"
)
VULKAN_LOADER_SHA1_FILE = "vulkan_loader.sha1"


def _build_linux():
    """Builds Vulkan-Loader from source using a self-contained Dockerfile for Linux."""
    docker_image_name = "vulkan-builder-glibc2.27:latest"
    dockerfile_name = "Dockerfile.vulkan_loader"
    if HOST_ARCH == "aarch64":
        docker_image_name = "vulkan-builder-glibc2.27-aarch64:latest"
        dockerfile_name = "Dockerfile.vulkan_loader.aarch64"

    logging.info(f"Extracting Vulkan Loader from Docker container using {dockerfile_name}...")
    container_name = "vulkan_loader_extractor"
    script_dir = Path(__file__).parent
    dockerfile_path = script_dir / dockerfile_name

    # 1. Build Docker image, passing the SHA as an argument
    logging.info(f"Building podman image: {docker_image_name}")
    subprocess.run(
        [
            "podman",
            "build",
            "--cgroup-manager=cgroupfs",
            "-t",
            docker_image_name,
            "-f",
            str(dockerfile_path),
            "--build-arg",
            f"VULKAN_LOADER_GIT_SHA={VULKAN_LOADER_GIT_SHA}",
            str(script_dir),
        ],
        check=True,
    )

    # 2. Create a container from the pre-built image.
    try:
        subprocess.run(
            ["podman", "create", "--name", container_name, docker_image_name],
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as e:
        logging.error("Failed to create podman container.")
        logging.error(f"podman image '{docker_image_name}' might not exist.")
        logging.error(
            "Build it first by running: podman build -t {} -f {} .".format(
                docker_image_name,
                Path(__file__).parent / "Dockerfile.vulkan_loader",
            )
        )
        logging.error(f"Stderr: {e.stderr}")
        sys.exit(1)

    try:
        # 3. Copy the built library and its symlinks from the container.
        container_lib_dir = "/opt/vulkan_loader/lib"
        os.makedirs(VULKAN_LOADER_PREBUILTS_PATH, exist_ok=True)

        logging.info(f"Copying {container_lib_dir} from container to {VULKAN_LOADER_PREBUILTS_PATH}")
        subprocess.run(
            ["podman", "cp", f"{container_name}:{container_lib_dir}/.", str(VULKAN_LOADER_PREBUILTS_PATH)],
            check=True,
        )
    finally:
        # 4. Clean up the temporary container.
        logging.info(f"Removing temporary container: {container_name}")
        subprocess.run(["podman", "rm", container_name], check=True, capture_output=True)

    # 5. Create the SHA1 file on the host.
    with open(VULKAN_LOADER_PREBUILTS_PATH / VULKAN_LOADER_SHA1_FILE, "w") as f:
        f.write(VULKAN_LOADER_GIT_SHA)

    logging.info("Successfully extracted Vulkan-Loader from Docker.")


def _build_native(args, prebuilts_out_dir):
    """Builds Vulkan-Loader from source natively for non-Linux hosts."""
    VULKAN_LOADER_OUT_FILES = [VULKAN_LOADER_SHA1_FILE]
    if HOST_OS == "darwin":
        VULKAN_LOADER_OUT_FILES.append("libvulkan.dylib")
    elif HOST_OS == "windows":
        VULKAN_LOADER_OUT_FILES.append("vulkan-1.dll")

    def installVulkanLoader(builddir, installdir):
        """Installs the output files from `builddir` to `installdir`."""
        logging.info("Installing Vulkan-Loader to %s", installdir)
        os.makedirs(installdir, exist_ok=True)

        for f in VULKAN_LOADER_OUT_FILES:
            src_file = builddir / f
            dst_file = installdir / f
            logging.info("Copy %s => %s", str(src_file), str(dst_file))
            if os.path.exists(dst_file):
                logging.info("Target file '%s' exists, deleting.", str(dst_file))
                os.remove(dst_file)
            shutil.copyfile(src_file, dst_file)
        logging.info("Installed Vulkan-Loader files to %s", installdir)

    # Determine which cmake executable to use.
    cmake_executable = None
    prebuilt_cmake_bin_path = Path(CMAKE_PATH)
    cmake_exe_name = "cmake.exe" if HOST_OS == "windows" else "cmake"
    prebuilt_cmake_exe = prebuilt_cmake_bin_path / cmake_exe_name

    if prebuilt_cmake_exe.exists():
        logging.info(f"Using prebuilt cmake: {prebuilt_cmake_exe}")
        cmake_executable = str(prebuilt_cmake_exe)
    else:
        logging.error(f"Prebuilt cmake not found at {prebuilt_cmake_bin_path}. Cannot proceed without prebuilt cmake.")
        return False # Return False on failure

    # Create a modified environment for subprocesses to find the prebuilt cmake.
    env = os.environ.copy()
    env["PATH"] = str(prebuilt_cmake_bin_path) + os.pathsep + env.get("PATH", "")

    logging.info("Building Vulkan-Loader (native)...")

    build_dir = Path(prebuilts_out_dir) / "vulkan-loader-build"
    clone_dir = build_dir / "Vulkan-Loader"

    # Always ensure a clean clone of the repository.
    if clone_dir.exists():
        logging.info(f"Removing existing clone directory: {clone_dir}")
        if HOST_OS == "windows":
            subprocess.run(["cmd", "/c", "rd", "/s", "/q", str(clone_dir)], check=True)
        else:
            subprocess.run(["rm", "-rf", str(clone_dir)], check=True)
    os.makedirs(clone_dir, exist_ok=True)

    logging.info(f"Cloning Vulkan-Loader repository from {VULKAN_LOADER_REPO_URL} to {clone_dir}...")
    subprocess.run(
        ["git", "clone", VULKAN_LOADER_REPO_URL, str(clone_dir)], check=True
    )
    subprocess.run(
        ["git", "checkout", VULKAN_LOADER_GIT_SHA], cwd=clone_dir, check=True
    )

    logging.info("Configuring Vulkan-Loader with CMake...")
    cmake_build_dir = clone_dir / "build"
    os.makedirs(cmake_build_dir, exist_ok=True)

    build_config = args.config.capitalize()

    cmake_cmd = [
        str(cmake_executable),
        "-S",
        ".",
        "-B",
        "build",
        "-D",
        "UPDATE_DEPS=On",
        f"-DCMAKE_BUILD_TYPE={build_config}",
    ]

    subprocess.run(cmake_cmd, cwd=clone_dir, check=True, env=env)

    logging.info("Building Vulkan-Loader...")
    subprocess.run(["cmake", "--build", "build", "--config", build_config], cwd=clone_dir, check=True, env=env)

    artifacts_dir = cmake_build_dir / "loader"
    if HOST_OS == "windows":
        artifacts_dir = artifacts_dir / build_config

    with open(artifacts_dir / VULKAN_LOADER_SHA1_FILE, "w") as f:
        f.write(VULKAN_LOADER_GIT_SHA)

    installVulkanLoader(artifacts_dir, VULKAN_LOADER_PREBUILTS_PATH)

    if args.dist:
        vulkan_loader_install_dir = Path(prebuilts_out_dir) / "vulkan_loader"
        installVulkanLoader(artifacts_dir, vulkan_loader_install_dir)

    logging.info("Successfully built Vulkan-Loader!")


def buildPrebuilt(args, prebuilts_out_dir, check_sha1=False):
    """Builds Vulkan-Loader from source."""

    # 1. Check if we need to build at all.
    sha1_file = VULKAN_LOADER_PREBUILTS_PATH / VULKAN_LOADER_SHA1_FILE
    if check_sha1 and sha1_file.exists():
        with open(sha1_file, "r") as f:
            prebuilt_sha = f.read().strip()
        if prebuilt_sha == VULKAN_LOADER_GIT_SHA:
            logging.info(
                "Prebuilt Vulkan-Loader SHA (%s) matches target SHA. Skipping build.",
                prebuilt_sha,
            )
            return

    # 2. Delegate to the appropriate build function based on the host OS.
    if HOST_OS == "linux":
        _build_linux()
    else:
        _build_native(args, prebuilts_out_dir)
