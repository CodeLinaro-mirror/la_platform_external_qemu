#!/usr/bin/env python3
# Copyright 2026 The Android Open Source Project
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
"""Unit tests for gen_linker_exports."""

from pathlib import Path
import tempfile
import unittest

import gen_linker_exports


class GenLinkerExportsTest(unittest.TestCase):

    def test_parse_def_file_filters_directives_comments_and_windows_only(self):
        def_content = (
            "LIBRARY qemu-system-x86_64.exe\r\n"
            "EXPORTS\r\n"
            "    ; Comment with Unicode 测试_🚀_é\r\n"
            "    address_space_rw\r\n"
            "    qemu_chr_open_win_con ; windows-only\r\n"
            "    error_setg_internal ; inline comment\r\n"
            "    address_space_rw\r\n"
        )
        self.assertEqual(
            gen_linker_exports.parse_def_file(def_content),
            ["address_space_rw", "error_setg_internal"],
        )

    def test_format_macos_exports_prepends_underscore(self):
        output = gen_linker_exports.format_macos_exports(["foo", "bar"])
        self.assertEqual(output, "_foo\n_bar\n")

    def test_format_linux_exports_emits_undefined_and_export_dynamic_symbol(self):
        output = gen_linker_exports.format_linux_exports(["foo", "bar"])
        self.assertEqual(
            output,
            "--undefined=foo\n"
            "--export-dynamic-symbol=foo\n"
            "--undefined=bar\n"
            "--export-dynamic-symbol=bar\n",
        )

    def test_format_linux_shared_exports_emits_extern_and_local_wildcard(self):
        output = gen_linker_exports.format_linux_shared_exports(["foo", "bar"])
        self.assertEqual(
            output,
            "EXTERN(\n"
            "  foo\n"
            "  bar\n"
            ")\n"
            "VERSION {\n"
            "  {\n"
            "    global:\n"
            "      foo;\n"
            "      bar;\n"
            "    local:\n"
            "      *;\n"
            "  };\n"
            "}\n",
        )

    def test_main_writes_output_files_and_Handles_empty_def(self):
        with tempfile.TemporaryDirectory(prefix="测试_🚀_é_") as tmp_dir:
            tmp_path = Path(tmp_dir)
            def_file = tmp_path / "test.def"
            def_file.write_text("EXPORTS\r\n  qemu_init\r\n", encoding="utf-8")

            macos_out = tmp_path / "macos.exp"
            self.assertEqual(
                gen_linker_exports.main(["--os=macos", str(def_file), "-o", str(macos_out)]),
                0,
            )
            self.assertEqual(macos_out.read_bytes(), b"_qemu_init\n")

            linux_shared_out = tmp_path / "linux_shared.lds"
            self.assertEqual(
                gen_linker_exports.main(
                    ["--os=linux", "--linkshared", str(def_file), "-o", str(linux_shared_out)]
                ),
                0,
            )
            self.assertEqual(
                linux_shared_out.read_bytes(),
                gen_linker_exports.format_linux_shared_exports(["qemu_init"]).encode("utf-8"),
            )

            empty_def = tmp_path / "empty.def"
            empty_def.write_text("EXPORTS\n", encoding="utf-8")
            self.assertEqual(
                gen_linker_exports.main(["--os=linux", str(empty_def), "-o", str(tmp_path / "out.lds")]),
                1,
            )

            self.assertEqual(
                gen_linker_exports.main(
                    ["--os=linux", str(tmp_path / "missing.def"), "-o", str(tmp_path / "out.lds")]
                ),
                1,
            )


if __name__ == "__main__":
    unittest.main()
