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

from aemu.configure.linux_builder import LinuxBuilder
from aemu.configure.libraries import BazelLib


class TrustyBuilder(LinuxBuilder):
    def meson_config(self):
        linux = super().meson_config()

        linux["-Daudio_drv_list"] = "default"
        linux["-Db_pie"] = "false"
        linux["-Dhexagon_idef_parser"] = "true"
        linux["-Dmodules"] = "disabled"
        linux["-Dpa"] = "disabled"
        linux["-Dprefer_static"] = "true"
        linux["-Drutabaga_gfx"] = "disabled"
        linux["-Dslirp"] = "enabled"
        linux["-Dtools"] = "disabled"
        linux["-Dvnc"] = "disabled"
        linux["-Dwerror"] = "false"

        return linux

    def packages(self):
        # Similar to linux, but using static dependencies.
        super().packages()

        # Next we have our dependencies.
        return [
            BazelLib("//third_party/dtc:libfdt", "1.6.0", {}),
            BazelLib("@glib//:gmodule-static", "2.77.2", {}),
            BazelLib(
                "@glib//:glib-static",
                "2.77.2",
                {
                    "name": "glib-2.0",
                    "link_flags": "-pthread",
                    "Requires": "pcre2, gmodule-static",
                },
            ),
            BazelLib("@zlib//:zlib", "1.2.10", {}),
            BazelLib("@pixman//:pixman-1", "0.42.3", {"Requires": "pixman_simd"}),
            BazelLib("@pixman//:pixman_simd", "0.42.3", {}),
            BazelLib("@pcre2//:pcre2", "10.42", {}),
        ]
