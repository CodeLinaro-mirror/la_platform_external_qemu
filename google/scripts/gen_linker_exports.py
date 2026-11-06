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
"""Generates macOS and Linux linker export files from a Windows .def file."""

import argparse
from collections.abc import Sequence
from pathlib import Path
import sys

_DEF_DIRECTIVES = frozenset({"EXPORTS", "LIBRARY"})
_WINDOWS_ONLY_MARKER = "windows-only"


def parse_def_file(content: str) -> list[str]:
    """Extracts POSIX-exported symbol names from a Windows .def file."""
    symbols: list[str] = []
    seen: set[str] = set()
    for raw_line in content.splitlines():
        body, sep, comment = raw_line.partition(";")
        if sep and _WINDOWS_ONLY_MARKER in comment:
            continue
        tokens = body.strip().split()
        if not tokens or tokens[0] in _DEF_DIRECTIVES:
            continue
        symbol = tokens[0]
        if symbol not in seen:
            seen.add(symbol)
            symbols.append(symbol)
    return symbols


def format_macos_exports(symbols: Sequence[str]) -> str:
    """Formats a macOS ld64 -exported_symbols_list file."""
    return "\n".join(f"_{sym}" for sym in symbols) + "\n"


def format_linux_exports(symbols: Sequence[str]) -> str:
    """Formats a GNU ld / lld response file exporting only the given symbols.

    Emits `--undefined=<sym>` (so symbols in static archives are retained even
    when unreferenced by the main executable) and `--export-dynamic-symbol=<sym>`
    (so only the listed symbols are exported in `.dynsym`, while still
    preserving Clang's `--dynamic-list` sanitizer runtime exports under
    ASAN/LSAN/TSAN/UBSAN).
    """
    lines: list[str] = []
    for sym in symbols:
        lines.append(f"--undefined={sym}")
        lines.append(f"--export-dynamic-symbol={sym}")
    return "\n".join(lines) + "\n"


def format_linux_shared_exports(symbols: Sequence[str]) -> str:
    """Formats a GNU ld / lld linker script exporting only the given symbols.

    For `-shared` libraries, `--export-dynamic-symbol` does not hide other
    default-visibility symbols. Instead, emit `EXTERN(...)` (so symbols in
    static archives are retained) and an anonymous `VERSION` block with
    `local: *;` so all unlisted symbols become hidden and eligible for
    `--gc-sections` and ThinLTO dead-stripping.
    """
    extern_lines = "\n".join(f"  {sym}" for sym in symbols)
    global_lines = "\n".join(f"      {sym};" for sym in symbols)
    return (
        f"EXTERN(\n{extern_lines}\n)\n"
        f"VERSION {{\n  {{\n    global:\n{global_lines}\n"
        "    local:\n      *;\n  };\n}\n"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate macOS or Linux linker export files from a Windows .def file."
    )
    parser.add_argument(
        "--os",
        choices=("macos", "linux"),
        required=True,
        help="Target OS format ('macos' for ld64 -exported_symbols_list, 'linux' for GNU ld script).",
    )
    parser.add_argument(
        "--linkshared",
        action="store_true",
        help=(
            "Target a shared library (-shared): on Linux, emit a GNU ld / lld "
            "version script with 'local: *;' instead of executable "
            "--export-dynamic-symbol flags (no-op on macOS, where "
            "-exported_symbols_list applies to both)."
        ),
    )
    parser.add_argument(
        "def_file",
        type=Path,
        help="Input Windows module-definition (.def) file.",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="Output linker export file path.",
    )
    args = parser.parse_args(argv)

    try:
        content = args.def_file.read_text(encoding="utf-8")
    except OSError as exc:
        print(
            f"Failed to read Windows .def file '{args.def_file}': {exc}. "
            "Verify that the win_def_file path exists and is readable.",
            file=sys.stderr,
        )
        return 1

    symbols = parse_def_file(content)
    if not symbols:
        print(
            f"Failed to generate linker exports from '{args.def_file}': "
            "no exported symbols were found. Add symbol names under EXPORTS in the .def file.",
            file=sys.stderr,
        )
        return 1

    if args.os == "macos":
        output_text = format_macos_exports(symbols)
    elif args.linkshared:
        output_text = format_linux_shared_exports(symbols)
    else:
        output_text = format_linux_exports(symbols)

    try:
        args.output.write_text(output_text, encoding="utf-8", newline="\n")
    except OSError as exc:
        print(
            f"Failed to write linker export file '{args.output}': {exc}. "
            "Verify that the output directory exists and is writable.",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
