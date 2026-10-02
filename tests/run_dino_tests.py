#!/usr/bin/env python3
"""Compile and run the platform-independent dinosaur application tests."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="dino-host-tests-") as directory:
        binary = Path(directory) / "test_dino_model"
        subprocess.run(
            compiler
            + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(root / "main"),
                str(root / "tests/test_dino_model.c"),
                str(root / "main/dino_model.c"),
                "-o", str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
