#!/bin/bash
# Copyright 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CUR_DIR="$SCRIPT_DIR"
WORKSPACE_ROOT=""
while [[ "$CUR_DIR" != "/" ]]; do
    if [[ -f "$CUR_DIR/build/bazel/toplevel.bazelrc" ]] || [[ -d "$CUR_DIR/.repo" ]]; then
        WORKSPACE_ROOT="$CUR_DIR"
        break
    fi
    CUR_DIR="$(dirname "$CUR_DIR")"
done

if [[ -z "$WORKSPACE_ROOT" ]]; then
    echo "Could not find workspace root containing MODULE.bazel" >&2
    exit 1
fi

cd "$WORKSPACE_ROOT" && bazel run @qemu//google/scripts/fetch_bazel:fetch_bazel -- "$@"
