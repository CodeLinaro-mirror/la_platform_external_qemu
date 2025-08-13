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
from aemu.configure.base_builder import QemuBuilder
from aemu.configure.libraries import BazelLib


class WindowsBuilder(QemuBuilder):
    def meson_config(self):
        features = super().meson_config()

        features["--vsenv"] = None
        features["-Daudio_drv_list"] = "dsound"
        features["-Davx2"] = "auto"
        features["-Davx512bw"] = "auto"
        features["-Ddsound"] = "enabled"
        features["-Dvhost_net"] = "enabled"
        features["-Dwhpx"] = "enabled"

        return features


    def packages(self):
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
                "@glib//:gmodule-static",
                "2.77.2",
                {
                    "name": "gmodule-export-2.0",
                    "cflags": "-DGMODULE_STATIC_COMPILATION",
                    "Requires": "pcre2",
                },
            ),
            BazelLib("@glib//:gnulib", "2.77.2", {}),
            BazelLib("@glib//:dirent", "2.77.2", {}),
            BazelLib("@zlib//:zlib", "1.2.10", {}),
            BazelLib(
                "@glib//:glib-static",
                "2.77.2",
                {
                    "name": "glib-2.0",
                    "Requires": "pcre2, zlib, gmodule-export-2.0, gnulib, dirent",
                    "link_name": "glib-2.0.lib",
                    "cflags": "-DGLIB_STATIC_COMPILATION",
                    "dll_ext": "",
                },
            ),
            BazelLib("@pixman//:pixman-1", "0.42.3", {"Requires": "pixman_simd"}),
            BazelLib("@pixman//:pixman_simd", "0.42.3", {}),
            BazelLib("@pcre2//:pcre2", "10.42", {}),
        ]

    def config_mak(self):
        return [
            "TARGET_DIRS=aarch64-softmmu riscv64-softmmu x86_64-softmmu",
            "GENISOIMAGE=False",
            "TCG_TESTS_TARGETS=x86_64-softmmu",
            "EXESUF=.exe",
        ]
