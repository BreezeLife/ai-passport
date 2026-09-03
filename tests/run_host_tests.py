#!/usr/bin/env python3
"""Compile and run the hardware-independent WorkBuddy tests with a host C compiler."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_FRAME_BUDGET_BYTES = 2048
MIN_LVGL_MEMORY_KILOBYTES = 48
MIN_MAIN_TASK_STACK_BYTES = 8192


def read_sdkconfig_integer(setting: str) -> int | None:
    """Read an integer assignment from sdkconfig.defaults."""
    prefix = f"{setting}="
    for line in (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return int(line.removeprefix(prefix))
    return None


def check_lvgl_memory_budget() -> None:
    """Keep enough LVGL heap for compressed CJK glyph rendering."""
    configured = read_sdkconfig_integer("CONFIG_LV_MEM_SIZE_KILOBYTES")
    if configured is None or configured < MIN_LVGL_MEMORY_KILOBYTES:
        raise RuntimeError(
            "sdkconfig.defaults must reserve at least "
            f"{MIN_LVGL_MEMORY_KILOBYTES} KB for LVGL; found {configured!r}"
        )


def check_main_task_stack_budget() -> None:
    """Reserve enough startup stack for the synchronous first-frame render."""
    configured = read_sdkconfig_integer("CONFIG_ESP_MAIN_TASK_STACK_SIZE")
    if configured is None or configured < MIN_MAIN_TASK_STACK_BYTES:
        raise RuntimeError(
            "sdkconfig.defaults must reserve at least "
            f"{MIN_MAIN_TASK_STACK_BYTES} bytes for the main task; found {configured!r}"
        )


def check_startup_display_sequence() -> None:
    """Keep the panel dark until the first WorkBuddy frame is fully flushed."""
    source = (ROOT / "main/main.c").read_text(encoding="utf-8")
    markers = [
        "bsp_display_init()",
        "wb_app_start(battery_available)",
        "bsp_lvgl_refresh_now()",
        "bsp_display_backlight(80U)",
    ]
    positions = [source.find(marker) for marker in markers]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise RuntimeError(
            "startup must initialize the display, render WorkBuddy, flush the first "
            "frame, then enable the backlight"
        )
    if source.count("bsp_display_backlight(") != 1:
        raise RuntimeError("startup must enable the display backlight exactly once")


def check_production_frame_budget(name: str, sources: list[str]) -> None:
    """Reject production functions that cannot fit safely in firmware task stacks."""
    compiler = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix=f"workbuddy-{name}-frames-") as tmp:
        for source in sources:
            object_path = Path(tmp) / f"{Path(source).stem}.o"
            command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-Wframe-larger-than={PRODUCTION_FRAME_BUDGET_BYTES}",
                "-Itests/shims",
                "-Imain",
                "-c",
                source,
                "-o",
                str(object_path),
            ]
            subprocess.run(command, cwd=ROOT, check=True)


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

    check_lvgl_memory_budget()
    check_main_task_stack_budget()
    check_startup_display_sequence()

    if "all" in requested or "ui" in requested:
        run_test("test_ui_pixel_math", ["tests/test_ui_pixel_math.c", "main/ui_pixel_math.c"])
    if "all" in requested or "model" in requested:
        check_production_frame_budget("model", ["main/workbuddy_model.c"])
        run_test(
            "test_workbuddy_model",
            ["tests/test_workbuddy_model.c", "main/workbuddy_model.c"],
        )
    if "all" in requested or "protocol" in requested:
        check_production_frame_budget("protocol", ["main/workbuddy_protocol.c"])
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
