# -*- coding: utf-8 -*-
# Copyright 2023 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the',  help='License');
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an',  help='AS IS' BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys

from aemu.configure.base_builder import QemuBuilder
from aemu.configure.libraries import BazelLib


class DarwinBuilder(QemuBuilder):
    def meson_config(self):
        features = super().meson_config()

        features["-Daudio_drv_list"] = "coreaudio"
        features["-Db_pie"] = "true"
        features["-Dcocoa"] = "enabled"
        features["-Dcoreaudio"] = "enabled"
        features["-Dhvf"] = "enabled"
        features["-Dvmnet"] = "enabled"

        return features

    def packages(self):
        """Set of bazel targets for which we are going to generate pkgconfig discovery files.

        Subclasses can override these to provide platform specific libraries/shims that might be needed.
        """
        return [
            BazelLib(
                "@crosvm//:rutabaga_c_ffi",
                "0.1.2",
                {
                    "name": "rutabaga_gfx_ffi",
                    "archive_target": "@crosvm//:rutabaga_ffi",
                },
            ),
            BazelLib("@dtc//:libfdt", "1.6.0", {}),
            BazelLib(
                "@glib//gmodule",
                "2.77.2",
                {
                    "name": "gmodule-export-2.0",
                    "link_flags": "-pthread",
                    "Requires": "pcre2",
                },
            ),
            BazelLib("@zlib//:zlib", "1.2.10", {}),
            BazelLib(
                "@glib//glib",
                "2.77.2",
                {
                    "name": "glib-2.0",
                    "Requires": "pcre2, gmodule-export-2.0",
                    "link_flags": "-liconv",
                },
            ),
            BazelLib("@pixman//:pixman-1", "0.42.3", {"Requires": "pixman_simd"}),
            BazelLib("@pixman//:pixman_simd", "0.42.3", {}),
            BazelLib("@pcre2//:pcre2", "10.42", {}),
        ]

    def config_mak(self):
        return [
            f"SRC_PATH={self.aosp / 'third_party' / 'qemu'}",
            "TARGET_DIRS=aarch64-softmmu riscv64-softmmu x86_64-softmmu",
            "GDB=",
            "SUBDIRS=",
            f"PYTHON={sys.executable} -B",
            f"MKVENV_ENSUREGROUP={sys.executable} -B {self.aosp}/third_party/qemu/python/scripts/mkvenv.py ensuregroup  --online",
            "GENISOIMAGE=",
            f"MESON={self.toolchain_generator.dest / 'meson'}",
            f"NINJA={self.toolchain_generator.dest / 'ninja'}",
            "EXESUF=",
            "CONFIG_DEFAULT_TARGETS=y",
            "TCG_TESTS_TARGETS=",
        ]
