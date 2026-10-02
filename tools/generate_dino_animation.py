#!/usr/bin/env python3
"""Convert the first eight complete cells of original 3x3 dinosaur atlases.

No images are invented or repainted. --partial produces previews only; the
firmware asset file is emitted only after every planned original is available.
--verify is read-only and checks source/converted bytes plus the real C decoder.
"""

import argparse
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageOps, __version__ as PILLOW_VERSION


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ASSETS = ROOT / "assets/animations/dinosaurs40"
WIDTH, HEIGHT, FRAME_COUNT, SPECIES_COUNT, COLORS = 144, 88, 8, 40, 16
PIXELS = WIDTH * HEIGHT
FRAME_BYTES = PIXELS // 2
PAPER = (247, 244, 233)
GIF_DURATIONS = [120, 130] * 4  # GIF centiseconds; average 125 ms / frame.


class AssetError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_plan(directory):
    raw = (directory / "generation-plan.json").read_bytes()
    plan = json.loads(raw)
    species = plan.get("species", [])
    ids = [item.get("id", "") for item in species]
    if (len(species) != SPECIES_COUNT or len(set(ids)) != SPECIES_COUNT or
            any(not re.fullmatch(r"[a-z][a-z0-9_]*", item) for item in ids) or
            plan.get("frames") != FRAME_COUNT or plan.get("grid") != [3, 3] or
            plan.get("width") != WIDTH or plan.get("height") != HEIGHT):
        raise AssetError("plan must specify 40 unique ids, 3x3, eight 144x88 frames")
    return plan, digest(raw)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb888(value):
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def unpack_rgb565(packed, palette):
    pixels = []
    for byte in packed:
        pixels.extend((palette[byte & 15], palette[byte >> 4]))
    return struct.pack("<" + "H" * len(pixels), *pixels)


def png_bytes(image):
    stream = io.BytesIO()
    image.save(stream, format="PNG", optimize=False)
    return stream.getvalue()


def convert_original(path):
    source = path.read_bytes()
    with Image.open(io.BytesIO(source)) as opened:
        opened.load()
        if opened.format != "PNG" or opened.width < 3 or opened.height < 3:
            raise AssetError(f"{path.name}: expected a PNG 3x3 atlas")
        atlas = opened.convert("RGBA")
    frames, crops, crop_hashes = [], [], []
    for index in range(FRAME_COUNT):
        row, col = divmod(index, 3)
        box = (col * atlas.width // 3, row * atlas.height // 3,
               (col + 1) * atlas.width // 3, (row + 1) * atlas.height // 3)
        cell = atlas.crop(box)
        crop_hashes.append(digest(cell.tobytes()))
        # Composite alpha and fit the whole cell; never trim limbs or background.
        opaque = Image.new("RGBA", cell.size, PAPER + (255,))
        opaque.alpha_composite(cell)
        fitted = ImageOps.contain(opaque.convert("RGB"), (WIDTH, HEIGHT),
                                 method=Image.Resampling.LANCZOS)
        frame = Image.new("RGB", (WIDTH, HEIGHT), PAPER)
        frame.paste(fitted, ((WIDTH - fitted.width) // 2,
                             (HEIGHT - fitted.height) // 2))
        frames.append(frame)
        crops.append(list(box))
    if len(set(crop_hashes)) != FRAME_COUNT:
        raise AssetError(f"{path.name}: the first eight source cells are not unique")

    stacked = Image.new("RGB", (WIDTH, HEIGHT * FRAME_COUNT))
    for index, frame in enumerate(frames):
        stacked.paste(frame, (0, index * HEIGHT))
    quantized = stacked.quantize(colors=COLORS, method=Image.Quantize.MEDIANCUT,
                                dither=Image.Dither.NONE)
    colors = quantized.getpalette()[:COLORS * 3]
    palette = [rgb565(*colors[i:i + 3]) for i in range(0, len(colors), 3)]
    # This one stacked quantization assigns one shared palette to all frames.
    indices = quantized.tobytes()
    if len(indices) != PIXELS * FRAME_COUNT or max(indices) >= COLORS:
        raise AssetError(f"{path.name}: quantizer emitted an invalid palette index")
    packed_frames, rendered, raw_frames = [], [], []
    for index in range(FRAME_COUNT):
        data = indices[index * PIXELS:(index + 1) * PIXELS]
        packed = bytes(data[i] | (data[i + 1] << 4) for i in range(0, PIXELS, 2))
        raw = unpack_rgb565(packed, palette)
        preview = Image.new("RGB", (WIDTH, HEIGHT))
        preview.putdata([rgb888(value) for (value,) in struct.iter_unpack("<H", raw)])
        packed_frames.append(packed)
        raw_frames.append(raw)
        rendered.append(preview)
    hashes = [digest(raw) for raw in raw_frames]
    if len(set(hashes)) != FRAME_COUNT:
        raise AssetError(f"{path.name}: 16-color conversion collapsed distinct frames")
    changed = []
    for index in range(FRAME_COUNT):
        first, second = raw_frames[index], raw_frames[(index + 1) % FRAME_COUNT]
        changed.append(sum(first[i:i + 2] != second[i:i + 2]
                           for i in range(0, len(first), 2)))
    return {
        "source_sha256": digest(source), "source_size": list(atlas.size),
        "crop_boxes": crops, "crop_sha256": crop_hashes,
        "palette": palette, "palette_bytes": struct.pack("<16H", *palette),
        "packed": b"".join(packed_frames), "raw_frames": raw_frames,
        "rendered": rendered, "changed_pixels": changed,
    }


def gif_bytes(rendered):
    # Keep the actual RGB565 colors rather than performing a second quantization.
    colors = list(dict.fromkeys(pixel for image in rendered for pixel in image.getdata()))
    lookup = {color: index for index, color in enumerate(colors)}
    palette = [component for color in colors for component in color]
    palette += [0] * (768 - len(palette))
    frames = []
    for image in rendered:
        frame = Image.new("P", image.size)
        frame.putpalette(palette)
        frame.putdata([lookup[pixel] for pixel in image.getdata()])
        frames.append(frame)
    stream = io.BytesIO()
    frames[0].save(stream, format="GIF", save_all=True, append_images=frames[1:],
                   duration=GIF_DURATIONS, loop=0, optimize=False, disposal=2)
    return stream.getvalue()


def make_c(entries):
    lines = ["/* Generated by tools/generate_dino_animation.py; do not edit. */",
             '#include "dino_animation.h"', "",
             '_Static_assert(DINO_COUNT == 40, "animation species count");',
             '_Static_assert(DINO_ANIMATION_FRAMES == 8, "animation frame count");',
             '_Static_assert(DINO_ANIMATION_FRAME_BYTES == 6336, "animation dimensions");', ""]
    for item, converted in entries:
        name = item["id"]
        lines.extend([f"static const uint16_t palette_{name}[16] = {{",
                      "    " + ", ".join(f"0x{value:04x}" for value in converted["palette"]) + ",",
                      "};", f"static const uint8_t frames_{name}[50688] = {{"])
        packed = converted["packed"]
        for start in range(0, len(packed), 24):
            lines.append("    " + ", ".join(f"0x{value:02x}" for value in packed[start:start + 24]) + ",")
        lines.extend(["};", ""])
    lines.append("const dino_animation_asset_t dino_animation_assets[DINO_COUNT] = {")
    for item, _ in entries:
        name = item["id"]
        lines.append(f"    {{palette_{name}, frames_{name}}},")
    lines.extend(["};", ""])
    return "\n".join(lines).encode()


def write_previews(directory, item, converted, index, partial):
    name = item["id"]
    subdir = directory / ("preview-partial" if partial else "preview")
    subdir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "id": name, "index": index,
        "source": {"path": f"originals/{name}.png", "sha256": converted["source_sha256"],
                   "size": converted["source_size"]},
        "crop_boxes": converted["crop_boxes"],
        "palette_rgb565": converted["palette"],
        "unique_frames": FRAME_COUNT,
        "adjacent_changed_pixels": converted["changed_pixels"], "frames": [],
    }
    for frame, rendered in enumerate(converted["rendered"]):
        data = png_bytes(rendered)
        path = subdir / f"{name}-{frame}.png"
        path.write_bytes(data)
        metadata["frames"].append({
            "frame": frame, "source_crop_sha256": converted["crop_sha256"][frame],
            "rgb565_sha256": digest(converted["raw_frames"][frame]),
            "preview": str(path.relative_to(directory)), "preview_sha256": digest(data),
        })
    data = gif_bytes(converted["rendered"])
    path = subdir / f"{name}.gif"
    path.write_bytes(data)
    metadata["gif"] = {"path": str(path.relative_to(directory)), "sha256": digest(data),
                       "durations_ms": GIF_DURATIONS, "loop_ms": sum(GIF_DURATIONS)}
    if not partial:
        converted_dir = directory / "converted"
        converted_dir.mkdir(parents=True, exist_ok=True)
        for key, suffix, data in (("packed", "i4", converted["packed"]),
                                  ("palette", "rgb565", converted["palette_bytes"])):
            path = converted_dir / f"{name}.{suffix}"
            path.write_bytes(data)
            metadata[key] = {"path": str(path.relative_to(directory)),
                             "sha256": digest(data), "bytes": len(data)}
    return metadata


def write_readmes(directory, metadata_name):
    english = f'''English · [简体中文](README.zh_CN.md)

# Dinosaur motion resources

The user requested all 40 species as original Image2 illustrations. The host image
tool received `gpt-image-2` requests; its observed model id is not exposed. The
[generation plan](generation-plan.json) retains this distinction. Original PNG
atlases remain unchanged in `originals/`; corresponding prompts remain in
`prompts/`. These are generated illustrations, not extracted movie footage.

`tools/generate_dino_animation.py` crops the first eight cells of each 3×3 atlas
in row order. It keeps each complete cell, composites alpha over cream, fits it
inside 144×88 with Lanczos resizing and letterboxing, then quantizes all eight
frames together to one 16-color RGB565 palette. It does not paint, trim subjects,
or synthesize movement. The ninth cell is intentionally ignored.

Each species occupies 50,688 I4 bytes and 32 palette bytes. All 40 total
2,028,800 payload bytes; the ESP32 descriptor table adds 320 bytes. Low nibbles
come first. The pure C decoder writes a 12,672-pixel (25,344-byte) RGB565 buffer.
The application targets eight frames per second, 125 ms per frame. GIF previews
alternate 120/130 ms because GIF timing uses centiseconds; each loop is 1 second.

Run `python3 tools/generate_dino_animation.py` only when all 40 originals exist.
It emits the packed resources, actual-color PNG/GIF previews, metadata and
`main/dino_animation_assets.c`. `--partial` emits existing previews and separate
partial metadata only; it never makes placeholder species or firmware arrays.
`--verify` is read-only. It reproduces conversion with the recorded Pillow
version, compares source/packed/palette/preview hashes, checks every GIF frame
and timing, and compiles the real C decoder to compare all 320 decoded frames.
The current resource record is [{metadata_name}]({metadata_name}).

Eight unique converted frames are required for every species. Pixel differences
prove that frame bytes differ; they do not establish anatomical accuracy,
believable gait, smooth loop seams, screen quality or real-device timing.
These remain visual and physical-device acceptance checks. Host previews are
not device photographs. Original illustrations still need human review.
'''
    chinese = f'''[English](README.md) · 简体中文

# 恐龙动作资源

用户要求为全部 40 种恐龙生成原创 Image2 插画。主机图像工具收到
`gpt-image-2` 请求，但未公开实际模型标识；[生成计划](generation-plan.json)
保留这一区别。`originals/` 中的原始 PNG 图集保持不变，对应提示词保存在
`prompts/`。这些是生成插画，并非从电影原片提取的画面。

`tools/generate_dino_animation.py` 按行截取每张 3×3 图集的前八格，
保留完整格子，在奶油色背景上合成透明区域，用 Lanczos 等比例缩放并留边，
放入 144×88 画布；随后将八帧一起量化为同一组 16 色 RGB565 调色板。
转换不重新绘图、不裁切主体，也不合成动作。第九格按计划忽略。

每种恐龙占 50,688 字节 I4 与 32 字节调色板；40 种共 2,028,800 字节，
ESP32 描述表另占 320 字节。低半字节先解码。纯 C 解码器写入
12,672 像素（25,344 字节）的 RGB565 缓冲区。应用目标为每秒八帧、每帧
125 毫秒。GIF 时间单位为百分之一秒，因此预览交替使用 120/130 毫秒，
每轮总长一秒。

全部 40 张原图齐备后运行 `python3 tools/generate_dino_animation.py`，
输出打包资源、实际颜色 PNG/GIF 预览、元数据与
`main/dino_animation_assets.c`。`--partial` 仅输出已有原图的预览及独立
部分元数据，绝不生成占位恐龙或固件数组。`--verify` 只读校验：使用记录的
Pillow 版本重现转换，比对原图、I4、调色板、预览哈希，逐帧检查 GIF 与时间，
并编译真实 C 解码器，对照全部 320 帧解码结果。
当前资源记录为 [{metadata_name}]({metadata_name})。

每种恐龙必须保留八个不同的转换后帧。像素差异只证明帧字节不同，不能证明
解剖准确、步态可信、循环衔接平滑、屏幕效果或实机帧率；这些仍需人工画面
审核与实机验收。主机预览并非设备实拍，原始插画仍需人工审核。
'''
    (directory / "README.md").write_text(english)
    (directory / "README.zh_CN.md").write_text(chinese)


def generate(directory, c_output, partial=False):
    plan, plan_hash = read_plan(directory)
    missing = [item["id"] for item in plan["species"]
               if not (directory / "originals" / f"{item['id']}.png").is_file()]
    if missing and not partial:
        raise AssetError(f"missing {len(missing)}/40 originals: " + ", ".join(missing))
    entries = []
    for item in plan["species"]:
        path = directory / "originals" / f"{item['id']}.png"
        if path.is_file():
            entries.append((item, convert_original(path)))
    # Validate all originals before producing any firmware source.
    metadata = {
        "format_version": 1, "complete": not partial, "species_count": len(entries),
        "planned_species_count": SPECIES_COUNT, "width": WIDTH, "height": HEIGHT,
        "frames": FRAME_COUNT, "palette_colors": COLORS, "packed_format": "I4 low nibble first",
        "palette_format": "RGB565 little endian", "frame_interval_ms": 125,
        "frame_bytes": FRAME_BYTES, "payload_bytes": len(entries) * (FRAME_BYTES * 8 + 32),
        "source_plan_sha256": plan_hash, "requested_model": plan.get("requested_model"),
        "observed_model_id": plan.get("observed_model_id"), "pillow_version": PILLOW_VERSION,
        "transform": "full-cell RGBA composite, contain Lanczos letterbox, shared median-cut 16 colors, no dither",
        "missing_originals": missing, "species": [],
    }
    by_id = {item["id"]: index for index, item in enumerate(plan["species"])}
    for item, converted in entries:
        metadata["species"].append(write_previews(directory, item, converted, by_id[item["id"]], partial))
    if not partial:
        data = make_c(entries)
        c_output.parent.mkdir(parents=True, exist_ok=True)
        c_output.write_bytes(data)
        metadata["generated_c_sha256"] = digest(data)
    metadata_name = "partial-metadata.json" if partial else "metadata.json"
    (directory / metadata_name).write_text(json.dumps(metadata, indent=2) + "\n")
    if not partial or not (directory / "metadata.json").exists():
        write_readmes(directory, metadata_name)
    return metadata


def require(condition, message):
    if not condition:
        raise AssetError(message)


def check_file(directory, item, expected_bytes=None):
    path = directory / item["path"]
    require(path.resolve().is_relative_to(directory.resolve()), "metadata path escapes asset directory")
    data = path.read_bytes()
    require(digest(data) == item["sha256"], f"hash mismatch: {item['path']}")
    if expected_bytes is not None:
        require(data == expected_bytes, f"converted byte mismatch: {item['path']}")
    if "bytes" in item:
        require(len(data) == item["bytes"], f"size mismatch: {item['path']}")
    return data


def check_c_decoder(c_output, entries):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="dino-animation-verify-") as temp:
        library = Path(temp) / ("decoder.dylib" if sys.platform == "darwin" else "decoder.so")
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                  "-shared", "-fPIC", "-I", str(ROOT / "main"),
                                  str(ROOT / "main/dino_animation.c"), str(c_output),
                                  "-o", str(library)], check=True, capture_output=True)
        decoder = ctypes.CDLL(str(library)).dino_animation_decode
        decoder.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint16), ctypes.c_size_t]
        decoder.restype = ctypes.c_bool
        out = (ctypes.c_uint16 * PIXELS)()
        for species, (_, converted) in enumerate(entries):
            for frame in range(FRAME_COUNT):
                require(decoder(species, frame, out, PIXELS), f"C decoder rejected {species}/{frame}")
                raw = struct.pack("<" + "H" * PIXELS, *out)
                require(raw == converted["raw_frames"][frame], f"C decode mismatch {species}/{frame}")


def verify(directory, c_output):
    plan, plan_hash = read_plan(directory)
    metadata = json.loads((directory / "metadata.json").read_text())
    require(metadata.get("complete") is True and metadata.get("species_count") == SPECIES_COUNT,
            "complete 40-species metadata is required")
    require(metadata.get("source_plan_sha256") == plan_hash, "generation plan hash mismatch")
    require(metadata.get("pillow_version") == PILLOW_VERSION,
            "use recorded Pillow version for reproducible source conversion")
    require(metadata.get("width") == WIDTH and metadata.get("height") == HEIGHT and
            metadata.get("frames") == FRAME_COUNT and metadata.get("palette_colors") == COLORS and
            metadata.get("frame_bytes") == FRAME_BYTES and metadata.get("frame_interval_ms") == 125 and
            metadata.get("packed_format") == "I4 low nibble first" and
            metadata.get("palette_format") == "RGB565 little endian" and
            metadata.get("payload_bytes") == SPECIES_COUNT * (FRAME_BYTES * 8 + 32) and
            metadata.get("missing_originals") == [], "invalid animation format metadata")
    recorded = metadata.get("species", [])
    require(len(recorded) == SPECIES_COUNT, "metadata must contain every planned species")
    entries = []
    for index, (item, record) in enumerate(zip(plan["species"], recorded)):
        name = item["id"]
        require(record.get("id") == name and record.get("index") == index,
                f"species order mismatch: {name}")
        require(record["source"]["path"] == f"originals/{name}.png", f"source path mismatch: {name}")
        source = check_file(directory, record["source"])
        converted = convert_original(directory / record["source"]["path"])
        require(digest(source) == converted["source_sha256"] and
                record["source"]["size"] == converted["source_size"] and
                record["crop_boxes"] == converted["crop_boxes"], f"source crop mismatch: {name}")
        require(record["palette_rgb565"] == converted["palette"], f"palette mismatch: {name}")
        packed = check_file(directory, record["packed"], converted["packed"])
        palette_bytes = check_file(directory, record["palette"], converted["palette_bytes"])
        require(len(packed) == FRAME_BYTES * FRAME_COUNT and len(palette_bytes) == 32,
                f"invalid converted length: {name}")
        require(record["unique_frames"] == FRAME_COUNT and
                record["adjacent_changed_pixels"] == converted["changed_pixels"],
                f"frame uniqueness metadata mismatch: {name}")
        frame_records = record["frames"]
        require(len(frame_records) == FRAME_COUNT, f"frame metadata count: {name}")
        for frame, frame_record in enumerate(frame_records):
            require(frame_record["frame"] == frame and
                    frame_record["source_crop_sha256"] == converted["crop_sha256"][frame] and
                    frame_record["rgb565_sha256"] == digest(converted["raw_frames"][frame]),
                    f"frame byte hash mismatch: {name}/{frame}")
            preview = check_file(directory, {"path": frame_record["preview"],
                                            "sha256": frame_record["preview_sha256"]})
            with Image.open(io.BytesIO(preview)) as image:
                require(image.size == (WIDTH, HEIGHT) and image.convert("RGB").tobytes() ==
                        converted["rendered"][frame].tobytes(), f"preview pixels mismatch: {name}/{frame}")
        gif_data = check_file(directory, record["gif"])
        require(record["gif"]["durations_ms"] == GIF_DURATIONS and
                record["gif"]["loop_ms"] == 1000, f"GIF timing metadata mismatch: {name}")
        with Image.open(io.BytesIO(gif_data)) as gif:
            require(gif.n_frames == FRAME_COUNT and gif.info.get("loop") == 0, f"GIF frame count/loop: {name}")
            for frame in range(FRAME_COUNT):
                gif.seek(frame)
                require(gif.info.get("duration") == GIF_DURATIONS[frame] and
                        gif.convert("RGB").tobytes() == converted["rendered"][frame].tobytes(),
                        f"GIF pixels/timing mismatch: {name}/{frame}")
        entries.append((item, converted))
    generated = c_output.read_bytes()
    require(digest(generated) == metadata.get("generated_c_sha256"), "generated C hash mismatch")
    require(generated == make_c(entries), "generated C data/order mismatch")
    check_c_decoder(c_output, entries)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--c-output", type=Path, default=ROOT / "main/dino_animation_assets.c")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--partial", action="store_true", help="only existing originals; no firmware C or packed assets")
    mode.add_argument("--verify", action="store_true", help="read-only verify all 40 species and every C-decoded frame")
    args = parser.parse_args()
    try:
        metadata = verify(args.assets, args.c_output) if args.verify else generate(args.assets, args.c_output, args.partial)
    except (AssetError, OSError, KeyError, TypeError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"dino animation: FAIL: {error}", file=sys.stderr)
        return 1
    state = "PARTIAL preview" if args.partial else "PASS"
    print(f"dino animation: {state}: {metadata['species_count']}/40 species, "
          f"{metadata['species_count'] * FRAME_COUNT} unique frames, "
          f"{metadata['payload_bytes']:,} payload bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
