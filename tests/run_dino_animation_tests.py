#!/usr/bin/env python3
"""Compile the platform-independent I4 decoder tests without production assets."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="dino-animation-tests-") as directory:
        binary = Path(directory) / "test_dino_animation"
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                  "-pedantic"] + flags + [
            "-I", str(root / "main"), str(root / "tests/test_dino_animation.c"),
            str(root / "main/dino_animation.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
