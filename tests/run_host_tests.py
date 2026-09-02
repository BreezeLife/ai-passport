#!/usr/bin/env python3
"""Compile and run the hardware-independent WorkBuddy tests with a host C compiler."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run_test(name: str, sources: list[str]) -> None:
    compiler = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix=f"workbuddy-{name}-") as tmp:
        executable = Path(tmp) / name
        command = [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Itests/shims",
            "-Imain",
            *sources,
            "-o",
            str(executable),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)


def main() -> int:
    arguments = sys.argv[1:]
    if arguments[:1] == ["--test"]:
        if len(arguments) != 2:
            print("usage: run_host_tests.py --test model|protocol|ui|all", file=sys.stderr)
            return 2
        requested = {arguments[1]}
    else:
        requested = set(arguments or ["all"])
    known = {"all", "model", "protocol", "ui"}
    unknown = requested - known
    if unknown:
        print(f"unknown test group: {', '.join(sorted(unknown))}", file=sys.stderr)
        return 2

    if "all" in requested or "ui" in requested:
        run_test("test_ui_pixel_math", ["tests/test_ui_pixel_math.c", "main/ui_pixel_math.c"])
    if "all" in requested or "model" in requested:
        run_test(
            "test_workbuddy_model",
            ["tests/test_workbuddy_model.c", "main/workbuddy_model.c"],
        )
    if "all" in requested or "protocol" in requested:
        run_test(
            "test_workbuddy_protocol",
            [
                "tests/test_workbuddy_protocol.c",
                "main/workbuddy_protocol.c",
                "main/workbuddy_model.c",
            ],
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
