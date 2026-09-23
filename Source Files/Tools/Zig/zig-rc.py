#!/usr/bin/env python3
"""Small CMake RC wrapper for Zig's Windows resource compiler."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys


def main() -> int:
    arguments = sys.argv[1:]
    if not arguments or arguments[0] in {"--version", "-version"}:
        print("Microsoft (R) Windows (R) Resource Compiler Version 10.0")
        return 0

    zig_name = os.environ.get("VECTORCLICK_ZIG", "zig")
    zig_path = shutil.which(zig_name) if not os.path.isabs(zig_name) else zig_name
    if not zig_path or not os.path.isfile(zig_path):
        print(
            "Vector Click Zig build: set VECTORCLICK_ZIG to the Zig 0.16.0 executable.",
            file=sys.stderr,
        )
        return 127

    # CMake places the input .rc path last. Zig's RC frontend accepts an
    # explicit `--` separator before an absolute resource path.
    if (
        arguments
        and os.path.isabs(arguments[-1])
        and arguments[-1].lower().endswith(".rc")
    ):
        arguments = [*arguments[:-1], "--", arguments[-1]]

    output_path: str | None = None
    for index, argument in enumerate(arguments):
        lowered = argument.lower()
        if lowered == "/fo" and index + 1 < len(arguments):
            output_path = arguments[index + 1]
            break
        if lowered.startswith("/fo") and len(argument) > 3:
            output_path = argument[3:]
            break

    input_path = arguments[-1] if arguments else None
    command = [zig_path, "rc", "/:auto-includes", "gnu", *arguments]
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        return completed.returncode

    # CMake's Ninja RC rule declares a depfile. Zig's resource compiler does
    # not create one, so record the direct .rc dependency after a successful
    # compile. Without this file Ninja treats the resource as perpetually
    # dirty and recompiles it before every link.
    if output_path and input_path:
        dependency_path = f"{output_path}.d"
        escaped_output = output_path.replace(" ", r"\ ")
        escaped_input = input_path.replace(" ", r"\ ")
        with open(dependency_path, "w", encoding="utf-8", newline="\n") as depfile:
            depfile.write(f"{escaped_output}: {escaped_input}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
