#!/usr/bin/env python3
"""Generate the reproducible WorkBuddy LVGL 9 Simplified Chinese font."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Iterable, Optional, Sequence, Set


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "main" / "fonts" / "lv_font_noto_sans_sc_14.c"
LICENSE = ROOT / "assets" / "fonts" / "NotoSansSC-OFL.txt"

FONT_NAME = "lv_font_noto_sans_sc_14"
FONT_SIZE = 14
FONT_BPP = 2
LV_FONT_CONV_VERSION = "1.5.3"

SOURCE_FILENAME = "NotoSansSC[wght].ttf"
SOURCE_URL = (
    "https://raw.githubusercontent.com/google/fonts/"
    "f6b2b7e8545e086ad3f821af21895d732b6485cf/"
    "ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
)
SOURCE_SHA256 = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"

PRINTABLE_ASCII = "".join(chr(codepoint) for codepoint in range(0x20, 0x7F))
CHINESE_PUNCTUATION = (
    "　、。·—–…‧，；：？！“”‘’（）［］｛｝〈〉《》「」『』【】〔〕〖〗"
)
PROJECT_UI_TEXT = (
    "WorkBuddy隐私封面收件箱消息未读任务产物新建同步状态回复录音转写确认发送"
    "成功失败离线演示连接刷新详情语音创建网络认证服务不可用过期规划进行等待"
    "已完成已归档已删除需要输入取消重试返回上一页下一页打开关闭"
)

_C_STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"', re.DOTALL)
_GLYPH_COMMENT_RE = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6}) ")


def gb2312_characters() -> Set[str]:
    """Return every distinct Unicode character decoded from GB2312 pairs."""

    characters: Set[str] = set()
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                decoded = bytes((lead, trail)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            characters.update(decoded)
    return characters


def _source_string_characters(main_dir: Path) -> Set[str]:
    """Collect non-ASCII characters from C string literals under main/."""

    characters: Set[str] = set()
    if not main_dir.is_dir():
        return characters

    for source in sorted(main_dir.rglob("*")):
        if source.suffix.lower() not in {".c", ".h"}:
            continue
        if "fonts" in source.relative_to(main_dir).parts:
            continue
        text = source.read_text(encoding="utf-8")
        for literal in _C_STRING_RE.findall(text):
            characters.update(char for char in literal[1:-1] if ord(char) > 0x7F)
    return characters


def build_character_set(root: Path, extra_text: str = "") -> Set[str]:
    """Build printable ASCII + GB2312 + punctuation + project UI coverage."""

    return (
        set(PRINTABLE_ASCII)
        | gb2312_characters()
        | set(CHINESE_PUNCTUATION)
        | set(PROJECT_UI_TEXT)
        | set(extra_text)
        | _source_string_characters(root / "main")
    )


def _candidate_converter_paths(explicit: Optional[Path]) -> Iterable[Path]:
    if explicit is not None:
        yield explicit.expanduser()
        return

    configured = os.environ.get("LV_FONT_CONV")
    if configured:
        yield Path(configured).expanduser()

    yield ROOT / "node_modules" / ".bin" / "lv_font_conv"

    discovered = shutil.which("lv_font_conv")
    if discovered:
        yield Path(discovered)


def resolve_converter(explicit: Optional[Path] = None) -> Path:
    """Resolve an explicit, configured, repository-local, or PATH converter."""

    for candidate in _candidate_converter_paths(explicit):
        resolved = candidate.resolve()
        if resolved.is_file():
            return resolved

    if explicit is not None:
        raise SystemExit(f"lv_font_conv not found at {explicit}")
    raise SystemExit(
        "lv_font_conv was not found; pass --lv-font-conv, set LV_FONT_CONV, "
        "install it on PATH, or install it in this repository's node_modules"
    )


def _converter_command(converter: Path) -> Sequence[str]:
    if os.access(converter, os.X_OK):
        return (str(converter),)
    if converter.suffix == ".js":
        node = shutil.which("node")
        if node:
            return (node, str(converter))
    raise SystemExit(f"lv_font_conv is not executable: {converter}")


def _run_checked(command: Sequence[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as error:
        details = (error.stderr or error.stdout or "no diagnostic output").strip()
        raise SystemExit(f"command failed ({error.returncode}): {details}") from error


def _verify_converter(converter_command: Sequence[str]) -> None:
    result = _run_checked((*converter_command, "--version"), cwd=ROOT)
    version = (result.stdout or result.stderr).strip()
    if version != LV_FONT_CONV_VERSION:
        raise SystemExit(
            f"lv_font_conv {LV_FONT_CONV_VERSION} is required for reproducibility; "
            f"found {version or 'unknown'}"
        )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _portable_output_name(output: Path) -> str:
    try:
        return output.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return output.name


def _postprocess_generated_source(
    generated: str,
    *,
    source_font: Path,
    temporary_output: Path,
    output: Path,
    character_count: int,
) -> str:
    include_block = (
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n'
        '#include "lvgl.h"\n'
        '#else\n'
        '#include "lvgl.h"\n'
        '#endif'
    )
    if include_block not in generated:
        raise SystemExit("unexpected lv_font_conv include block; refusing to write output")

    generated = generated.replace(include_block, "#include <lvgl.h>", 1)
    generated = generated.replace(str(source_font), SOURCE_FILENAME)
    generated = generated.replace(str(temporary_output), _portable_output_name(output))

    provenance = (
        " * WorkBuddy Simplified Chinese UI font.\n"
        f" * Source: {SOURCE_URL}\n"
        f" * Source SHA-256: {SOURCE_SHA256}\n"
        " * License: SIL Open Font License 1.1; see "
        "assets/fonts/NotoSansSC-OFL.txt.\n"
        f" * Character coverage: {character_count} Unicode code points.\n"
        " * Generated by tools/generate_workbuddy_font.py; do not edit.\n"
        " *\n"
    )
    marker = "/*******************************************************************************\n"
    if not generated.startswith(marker):
        raise SystemExit("unexpected lv_font_conv header; refusing to write output")
    generated = generated.replace(marker, marker + provenance, 1)
    return generated.rstrip("\n") + "\n"


def _verify_generated_source(generated: str, expected: Set[str]) -> int:
    if generated.count("#include <lvgl.h>") != 1 or '#include "lvgl.h"' in generated:
        raise SystemExit("generated source does not use exactly one #include <lvgl.h>")
    if not re.search(r"\bconst\s+lv_font_t\s+" + FONT_NAME + r"\s*=", generated):
        raise SystemExit(f"generated source does not export const lv_font_t {FONT_NAME}")
    if not re.search(r"\.bpp\s*=\s*2\s*,", generated):
        raise SystemExit("generated source is not 2 bpp")

    glyph_codepoints = {
        int(match, 16) for match in _GLYPH_COMMENT_RE.findall(generated)
    }
    missing = sorted(ord(character) for character in expected if ord(character) not in glyph_codepoints)
    if missing:
        preview = ", ".join(f"U+{codepoint:04X}" for codepoint in missing[:12])
        suffix = " ..." if len(missing) > 12 else ""
        raise SystemExit(f"source font is missing {len(missing)} requested glyphs: {preview}{suffix}")
    return len(glyph_codepoints)


def generate_font(
    source_font: Path,
    converter: Path,
    output: Path,
    extra_text: str = "",
) -> tuple[int, int, str]:
    source_font = source_font.expanduser().resolve()
    if not source_font.is_file():
        raise SystemExit(f"source font not found: {source_font}")
    actual_hash = _sha256(source_font)
    if actual_hash != SOURCE_SHA256:
        raise SystemExit(
            f"unexpected source font SHA-256: {actual_hash}; expected {SOURCE_SHA256} "
            f"from {SOURCE_URL}"
        )
    if not LICENSE.is_file():
        raise SystemExit(f"missing redistribution license: {LICENSE.relative_to(ROOT)}")

    converter_command = _converter_command(converter)
    _verify_converter(converter_command)
    characters = build_character_set(ROOT, extra_text)
    non_ascii_symbols = "".join(sorted(characters - set(PRINTABLE_ASCII), key=ord))

    with tempfile.TemporaryDirectory(prefix="workbuddy-font-") as temporary_dir:
        temporary_output = Path(temporary_dir) / output.name
        command = (
            *converter_command,
            "--size",
            str(FONT_SIZE),
            "--bpp",
            str(FONT_BPP),
            "--format",
            "lvgl",
            "--font",
            str(source_font),
            "-r",
            "0x20-0x7E",
            "--symbols",
            non_ascii_symbols,
            "--no-kerning",
            "--lv-include",
            "lvgl.h",
            "--lv-font-name",
            FONT_NAME,
            "-o",
            str(temporary_output),
        )
        _run_checked(command, cwd=ROOT)
        generated = temporary_output.read_text(encoding="utf-8")
        generated = _postprocess_generated_source(
            generated,
            source_font=source_font,
            temporary_output=temporary_output,
            output=output,
            character_count=len(characters),
        )
        glyph_count = _verify_generated_source(generated, characters)

        output.parent.mkdir(parents=True, exist_ok=True)
        staged_output = output.with_suffix(output.suffix + ".tmp")
        with staged_output.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(generated)
        os.replace(staged_output, output)

    return glyph_count, output.stat().st_size, _sha256(output)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate the 14 px, 2 bpp WorkBuddy LVGL font from the pinned "
            "Noto Sans SC source. This command never downloads dependencies."
        )
    )
    parser.add_argument("font", type=Path, help=f"local path to {SOURCE_FILENAME}")
    parser.add_argument(
        "--lv-font-conv",
        type=Path,
        help=(
            f"path to lv_font_conv {LV_FONT_CONV_VERSION}; if omitted, use "
            "LV_FONT_CONV, repository node_modules, or PATH"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=OUTPUT,
        help=f"output C path (default: {OUTPUT.relative_to(ROOT)})",
    )
    parser.add_argument(
        "--extra-text",
        action="append",
        default=[],
        metavar="TEXT",
        help="additional runtime text to include; may be repeated",
    )
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    converter = resolve_converter(args.lv_font_conv)
    output = args.output if args.output.is_absolute() else (ROOT / args.output)
    glyph_count, output_size, output_hash = generate_font(
        args.font,
        converter,
        output.resolve(),
        "".join(args.extra_text),
    )
    print(f"generated: {_portable_output_name(output)}")
    print(f"characters: {glyph_count} (GB2312 double-byte set: {len(gb2312_characters())})")
    print(f"bytes: {output_size}")
    print(f"sha256: {output_hash}")


if __name__ == "__main__":
    main()
