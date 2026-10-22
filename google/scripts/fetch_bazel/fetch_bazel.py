#!/usr/bin/env python3
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
import argparse
import json
import logging
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

import oauth2client.client
from ab.android_build_client import AndroidBuildClient, obtain_token
from ab.fetch_artifact import unzip_artifact
from ab.log import configure_logging
from tqdm import tqdm


PRESUBMIT_BUILD_TASKS = {
    "emulator_linux_x64:git_emu-main-next": "lin_bid",
    "emulator_linux_aarch64:git_emu-main-next": "lin_arm_bid",
    "emulator_windows_x64:git_emu-main-next": "win_bid",
    "emulator_mac_aarch64:git_emu-main-next": "mac_bid",
}

OPTIONAL_PRESUBMIT_BUILD_TASKS = {
    "emulator_linux_aarch64:git_emu-main-next",
}


class CustomFormatter(
    argparse.RawTextHelpFormatter, argparse.ArgumentDefaultsHelpFormatter
):
    pass


def parse_cl_and_revision(cl_arg: str) -> tuple[str, str]:
    """Extracts numeric change ID and revision (defaulting to 'current') from a CL string or URL."""
    match = re.search(r"(?:[+/]|^)(\d+)(?:/(\d+))?/?$", cl_arg.strip())
    if match:
        change_id = match.group(1)
        revision = match.group(2) or "current"
        return change_id, revision
    return cl_arg.strip(), "current"


def fetch_presubmit_bids(cl: str, revision: str, token: str) -> dict[str, str]:
    """Fetches presubmit build IDs (P...) for the given CL from Gerrit's Treetop plugin."""
    logging.info("Querying Gerrit Treetop for presubmit builds on CL %s (revision %s)...", cl, revision)
    raw = None
    for req_attempt in range(2):
        url = (
            f"https://googleplex-android-review.googlesource.com/a/changes/{cl}"
            f"/revisions/{revision}/treetop~presubmittasks?access_token={token}"
        )
        req = urllib.request.Request(url)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                raw = resp.read().decode("utf-8")
            break
        except urllib.error.HTTPError as e:
            if req_attempt == 0 and e.code in (400, 401, 403):
                logging.warning("HTTP %d from Gerrit Treetop; refreshing oauth2l token...", e.code)
                token = obtain_token("gerritcodereview", reset=True)
                continue
            raise RuntimeError(
                f"Failed to query Gerrit Treetop presubmit tasks for CL {cl}: {e}"
            ) from e
        except Exception as e:
            raise RuntimeError(
                f"Failed to query Gerrit Treetop presubmit tasks for CL {cl}: {e}"
            ) from e

    if raw.startswith(")]}'"):
        raw = raw[4:]
    data = json.loads(raw)
    tasks = data[0] if data and isinstance(data[0], list) else []

    candidates = {}
    for task in tasks:
        # check_name is at index 11 in PresubmitTask JSPB
        check_name = task[11] if len(task) > 11 and isinstance(task[11], str) else None
        if check_name in PRESUBMIT_BUILD_TASKS:
            attempt = task[2] if len(task) > 2 and isinstance(task[2], int) else 0
            status = task[3] if len(task) > 3 and isinstance(task[3], int) else 0
            # Extract P<digits> build ID from task resources (index 10) or serialized task
            resources_str = json.dumps(task[10] if len(task) > 10 else task)
            is_obsolete = (status == 5) or ("Obsolete" in resources_str)
            match = re.search(r"/builds/(?:build-details/)?(P\d+)", resources_str)
            if not match:
                match = re.search(r"\b(P\d+)\b", resources_str)
            if match:
                bid = match.group(1)
                bid_num = int(bid[1:])
                sort_key = (not is_obsolete, attempt, bid_num)
                arg_name = PRESUBMIT_BUILD_TASKS[check_name]
                if arg_name not in candidates or sort_key > candidates[arg_name][0]:
                    candidates[arg_name] = (sort_key, bid, check_name, attempt)

    found_bids = {}
    for arg_name, (_, bid, check_name, attempt) in candidates.items():
        found_bids[arg_name] = bid
        logging.info("Found build ID %s for %s (attempt %d)", bid, check_name, attempt)

    return found_bids


def run(cmd, dry_run: bool, cwd: Path = None):
    """Executes a command in a specified working directory.

    Args:
        cmd: A list of strings representing the command to execute.
        cwd: An optional Path object representing the working directory. If
            None, the current working directory is used.

    Raises:
        subprocess.CalledProcessError: If the command exits with a non-zero
            return code.
    """
    if cwd:
        cwd = Path(cwd).absolute()

    cmd = [str(c) for c in cmd]
    if dry_run:
        logging.warning("Dry run! Skipping: %s", " ".join(cmd))
        return

    logging.warning("Running: %s in %s", " ".join(cmd), cwd)
    subprocess.run(
        cmd,
        cwd=cwd,
    )


def git_commit(destination_dir, args):
    """Creates a Git commit with a message summarizing the bazel build update.

    Args:
        destination_dir: The git directory where we are going to commit
        args: An argparse.Namespace object containing additional arguments:
            - dest: The destination directory for the update.
            - branch: The Git branch to commit to.
            - prefix: The prefix for the commit message.
            - targets: The target files for the update.
            - reviewers: The reviewers for the commit.
            - artifact: The artifact to commit.
    """
    commit_header = f"Update bazel build from http://go/ag/{args.cl}"

    # Add a commit_msg footer if buganizer_id is specified
    commit_footer = f"\nBug: {args.buganizer_id}" if args.buganizer_id else ""
    lin_arm_bullet = f"\n- go/ab/{args.lin_arm_bid}" if getattr(args, "lin_arm_bid", None) else ""
    lin_arm_flag = f"            --lin-arm-bid {args.lin_arm_bid} \\\n" if getattr(args, "lin_arm_bid", None) else ""

    commit_msg = f"""

{commit_header}

This updates the bazel build from the running the meson generator on build bots.

The build is based on http://go/ag/{args.cl}, with presubmit build targets:

- go/ab/{args.mac_bid}
- go/ab/{args.win_bid}
- go/ab/{args.lin_bid}{lin_arm_bullet}

You can recreate the commit by running:

```sh
            {Path(sys.argv[0]).name} \\
            --dest {args.dest} \\
            --cl {args.cl} \\
            --mac-bid {args.mac_bid} \\
            --win-bid {args.win_bid} \\
            --lin-bid {args.lin_bid} \\
{lin_arm_flag}            --re {args.reviewers} \\

```
{commit_footer}
            """

    run(
        ["git", "-C", str(destination_dir), "commit", "-m", commit_msg],
        dry_run=args.dry_run,
    )


def obtain(args, target: str, platform: str, bid: str, dest: Path):
    """Obtains a specified target for a given build and integrates it into a Git repository.

    Args:
        args: An argparse.Namespace object containing the following arguments:
            - token: The authentication token for the Android Build Client.
        bid: The Build ID (BID) of the target to obtain.
        target: The name of the target to obtain.
        platform: The platform of interest
        dest: The directory where we should unzip this
    """
    logging.info("Obtaining target %s from build: %s", target, bid)
    ab_client = AndroidBuildClient(args.token)

    artifact = f"bazel-{platform}-{bid}.zip"
    unzip_artifact(
        ab_client,
        dest,
        target,
        artifact,
        bid,
    )


def fix_up_prefix(dest_dir: Path):
    """
    Removes occurrences of build-specific prefixes from files within a directory.

    This function first identifies uniqueprefix paths defined in 'config-host.h'
    files within the specified directory. It then finds all non-hidden files in the
    directory and replaces any occurrence of these prefixes with the string "rel_dir".

    Note: This is *a good enough* solution for the few header files we have.

    Args:
        dest_dir (Path): The directory in which to search for and modify files.

    Returns:
        None

    Raises:
        FileNotFoundError: If the specified directory or any files within it
                           cannot be found.
        PermissionError: If there are insufficient permissions to read or write files.
        IOError: If there's a general error during file operations.
    """

    prefix_paths = []
    config_files = dest_dir.rglob("config-host.h")

    for config_file in config_files:
        with open(config_file, "r") as f:
            for line in f:
                match = re.search(r'^#define\s+CONFIG_PREFIX\s+"([^"]+)"', line.strip())
                if match:
                    unique_prefix_path = match.group(1)
                    prefix_paths.append(unique_prefix_path)
                    break

    all_files = dest_dir.rglob("*")
    for file_path in tqdm(all_files):
        if file_path.is_file():
            try:
                with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
                    file_contents = f.read()
            except Exception as e:
                logging.warning("Could not read file %s: %s", file_path, e)
                continue

            # Track if any changes were made to this file
            file_modified = False
            for prefix_path in prefix_paths:
                if prefix_path in file_contents:
                    file_contents = file_contents.replace(prefix_path, "rel_dir")
                    file_modified = True

            # Only write back to the file if it was modified
            if file_modified:
                with open(file_path, "w", encoding="utf-8") as f:
                    f.write(file_contents)


def fetch_and_merge(args):
    targets = {
        "emulator_windows_x64": ("windows-x86_64", args.win_bid),
        "emulator_mac_aarch64": ("darwin-arm64", args.mac_bid),
        "emulator_linux_x64": ("linux-x86_64", args.lin_bid),
    }
    if getattr(args, "lin_arm_bid", None):
        targets["emulator_linux_aarch64"] = ("linux-aarch64", args.lin_arm_bid)

    dest = Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)

    # Obtain all the pieces
    for target, (platform, bid) in targets.items():
        obtain(args, target, platform, bid, dest)

    # Next merge them all together
    qemu = Path(args.aosp) / "third_party" / "qemu"
    merge = qemu / "google" / "scripts" / "merge_bazel.py"

    merge_cmd = [
        sys.executable,
        str(merge),
        "--buildfile",
        str(dest / "platform" / "BUILD.windows-x86_64"),
        "@platforms//os:windows",
        "--buildfile",
        str(dest / "platform" / "BUILD.darwin-arm64"),
        "@platforms//os:macos",
        "--buildfile",
        str(dest / "platform" / "BUILD.linux-x86_64"),
        "@goldfish_build//platforms:is_linux_x64",
        "--buildfile",
        str(dest / "platform" / "BUILD.linux-aarch64"),
        "@goldfish_build//platforms:is_linux_aarch64",
        "--verbatim-buildfile",
        str(qemu / "google" / "build" / "BUILD.common"),
        "--unique",
        "win_def_file",
        "-o",
        str(dest / "BUILD.bazel"),
    ]
    logging.info("Merging using: %s", " ".join(merge_cmd))
    subprocess.check_call(merge_cmd)
    logging.info("Running buildifier on %s", str(dest / "BUILD.bazel"))
    subprocess.check_call(["buildifier", str(dest / "BUILD.bazel")])

    # Let's remove all build bot specific information.
    fix_up_prefix(dest / "platform")

    run(["git", "-C", str(dest), "add", "."], dry_run=args.dry_run)


def find_aosp_root(start_directory=Path(__file__).resolve()) -> Path:
    current_directory = Path(start_directory).resolve()

    while True:
        repo_directory = current_directory / ".repo"
        if repo_directory.is_dir():
            return current_directory

        # Move up one directory
        parent_directory = current_directory.parent

        # Check if we've reached the root directory
        if current_directory == parent_directory:
            return current_directory

        current_directory = parent_directory


def main():
    parser = argparse.ArgumentParser(
        formatter_class=CustomFormatter,
        description="""
        This tool can be used to fetch the generated bazel files from the meson generator. The tool expects you to have
        ~/go/bin/oauth2l installed. You can find details on how to install this tool here: http://go/oauth2l



        Note: Token generation is only supported on glinux. For mac you will manually have to create tokens on your glinux machine by running:

        ~/go/bin/oauth2l fetch --sso $USER@google.com androidbuild.internal
        ~/go/bin/oauth2l fetch --sso $USER@google.com gerritcodereview
        """,
        epilog="""Examples:

        fetch-bazel --cl 41488125
        fetch-bazel --cl 306320 --mac-bid P74111280 --win-bid P74150599 --lin-bid P73982148
        """,
    )
    parser.add_argument(
        "--dest",
        type=str,
        default=find_aosp_root() / "third_party" / "qemu",
        help="Destination path for the final BUILD.bazel file (usually $AOSP/third_party/qemu)",
    )
    parser.add_argument(
        "--aosp",
        type=str,
        default=find_aosp_root(),
        help="AOSP root",
    )
    parser.add_argument(
        "--cl",
        type=str,
        required=True,
        help="The gerrit cl used to generate these headers.",
    )
    parser.add_argument(
        "--token",
        type=str,
        help="Apiary OAuth2 token for Android Build. If not set we will try to obtain one",
    )
    parser.add_argument(
        "--gerrit-token",
        type=str,
        help="OAuth2 token for Gerrit. If not set we will try to obtain one",
    )
    parser.add_argument(
        "--win-bid",
        type=str,
        required=False,
        default=None,
        help="Windows build from which we will fetch bazel-windows-x86_64-{win-bid}.zip (auto-detected from Gerrit if omitted)",
    )
    parser.add_argument(
        "--lin-bid",
        type=str,
        required=False,
        default=None,
        help="Linux build from which we will fetch bazel-linux-x86_64-{lin-bid}.zip (auto-detected from Gerrit if omitted)",
    )
    parser.add_argument(
        "--lin-arm-bid",
        type=str,
        required=False,
        default=None,
        help="Linux ARM64 build from which we will fetch bazel-linux-aarch64-{lin-arm-bid}.zip (auto-detected from Gerrit if omitted)",
    )
    parser.add_argument(
        "--mac-bid",
        type=str,
        required=False,
        default=None,
        help="Darwin build from which we will fetch bazel-darwin-arm64-{mac-bid}.zip (auto-detected from Gerrit if omitted)",
    )
    parser.add_argument(
        "--no-repo", action="store_true", help="Do not use any repo commands"
    )

    parser.add_argument(
        "--re",
        default="whollins@google.com,jansene@google.com,rkir@google.com,jpcottin@google.com",
        dest="reviewers",
        type=str,
        help="Set of reviewers for the gerrit change that will be created.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Dry run, only obtain artifacts, do not use git or repo.",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        dest="verbose",
        default=False,
        action="store_true",
        help="Verbose logging",
    )
    parser.add_argument(
        "--buganizer-id",
        type=str,
        help="Include Buganizer ID in the gerrit commit description ",
    )

    args = parser.parse_args()

    lvl = logging.DEBUG if args.verbose else logging.INFO
    configure_logging(lvl)

    cl, revision = parse_cl_and_revision(args.cl)
    args.cl = cl

    # Auto-detect missing build IDs from Gerrit Treetop presubmit tasks
    if not (args.win_bid and args.lin_bid and args.mac_bid):
        gerrit_token = args.gerrit_token or obtain_token("gerritcodereview")
        found_bids = fetch_presubmit_bids(args.cl, revision, gerrit_token)
        if not args.lin_bid:
            args.lin_bid = found_bids.get("lin_bid")
        if not args.lin_arm_bid:
            args.lin_arm_bid = found_bids.get("lin_arm_bid")
        if not args.win_bid:
            args.win_bid = found_bids.get("win_bid")
        if not args.mac_bid:
            args.mac_bid = found_bids.get("mac_bid")

        missing = [
            task
            for task, attr in PRESUBMIT_BUILD_TASKS.items()
            if task not in OPTIONAL_PRESUBMIT_BUILD_TASKS and not getattr(args, attr)
        ]
        if missing:
            raise ValueError(
                f"Could not automatically find presubmit build IDs for {', '.join(missing)} "
                f"on CL {args.cl} (revision {revision}). Please specify missing --*-bid flags explicitly."
            )

    # Ensure we have a valid Android Build OAuth token up front
    ab_client = AndroidBuildClient(args.token)
    args.token = ab_client.token

    # Make sure we have a destination that works
    destination_dir = Path(args.dest)
    if destination_dir.exists() and not destination_dir.is_dir():
        logging.fatal("Destination %s exists and is not a directory", destination_dir)
        sys.exit(1)

    logging.info("Preparing %s", destination_dir)
    destination_dir.mkdir(parents=True, exist_ok=True)
    try:
        if not args.no_repo:
            run(
                ["repo", "start", f"update-bazel-{args.cl}"],
                dry_run=args.dry_run,
                cwd=destination_dir,
            )

        fetch_and_merge(args)

        git_commit(destination_dir, args)

        if not args.no_repo:
            logging.info("Created change.. Uploading to gerrit")
            run(
                [
                    "repo",
                    "upload",
                    "-y",
                    "--label",
                    "Presubmit-Ready+1",
                    f"--br=update-bazel-{args.cl}",
                    f"--re={args.reviewers}",
                    str(destination_dir),
                ],
                dry_run=args.dry_run,
                cwd=destination_dir,
            )
    except oauth2client.client.AccessTokenCredentialsError as err:
        logging.fatal(
            "Token failure. You might have to clear your cache with ~/go/bin/oauth2l reset. (%s)",
            err,
        )
    except ValueError as err:
        logging.fatal("%s", err)
    finally:
        # Clean up repo branch if any
        if not args.no_repo:
            run(
                ["repo", "abandon", f"update-bazel-{args.cl}"],
                dry_run=args.dry_run,
                cwd=destination_dir,
            )


if __name__ == "__main__":
    main()
