#!/usr/bin/env python3
"""Resize generated originals and encode the application's LVGL 9 RGB565 assets."""

import hashlib
import json
from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[3]
ASSETS = Path(__file__).resolve().parent
WIDTH = 216
HEIGHT = 130
STRIDE = WIDTH * 2
DATA_SIZE = STRIDE * HEIGHT
BACKGROUND = (0xF7, 0xF4, 0xE9)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    manifest = json.loads((ASSETS / "prompts.json").read_text(encoding="utf-8"))
    entries = manifest["images"]
    if len(entries) != 8 or [entry["index"] for entry in entries] != list(range(8)):
        raise ValueError("Expected exactly eight ordered dinosaur entries")

    code = [
        '/* Generated from assets/images/dinosaurs/originals; do not hand-edit. */',
        '#include "dino_images.h"',
        '#include <stdint.h>',
        '',
    ]
    descriptors = []
    checksums = []
    for entry in entries:
        original = ASSETS / entry["original"]
        preview = ASSETS / entry["preview"]
        encoded = ASSETS / entry["rgb565"]
        with Image.open(original) as source:
            original_size = source.size
            resized = ImageOps.contain(source.convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
            image = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)
            image.paste(resized, ((WIDTH - resized.width) // 2, (HEIGHT - resized.height) // 2))
            image.save(preview, format="PNG", optimize=True)
            data = bytearray()
            for r, g, b in image.getdata():
                pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                data.extend((pixel & 0xFF, pixel >> 8))
        if len(data) != DATA_SIZE:
            raise ValueError("Incorrect RGB565 data size")
        encoded.write_bytes(data)
        symbol = "dino_" + entry["id"] + "_data"
        code.append("static const uint8_t " + symbol + "[" + str(DATA_SIZE) + "] __attribute__((aligned(4))) = {")
        for offset in range(0, len(data), 16):
            code.append("    " + ", ".join("0x%02x" % value for value in data[offset:offset + 16]) + ",")
        code.extend(["};", ""])
        descriptors.append(
            "    {\n"
            "        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n"
            "                   .flags = 0, .w = 216, .h = 130, .stride = 432},\n"
            "        .data_size = 56160,\n"
            "        .data = " + symbol + ",\n"
            "    },"
        )
        checksums.append({
            "index": entry["index"], "id": entry["id"],
            "original": entry["original"], "original_size": list(original_size),
            "original_sha256": sha256(original),
            "preview": entry["preview"], "preview_sha256": sha256(preview),
            "rgb565": entry["rgb565"], "rgb565_sha256": sha256(encoded),
            "rgb565_bytes": len(data),
        })
    code.append("const lv_image_dsc_t dino_images[8] = {")
    code.extend(descriptors)
    code.extend(["};", ""])
    (ROOT / "main" / "dino_images.c").write_text("\n".join(code), encoding="utf-8")
    (ROOT / "main" / "dino_images.h").write_text(
        "/* Generated dinosaur artwork descriptors for LVGL 9. */\n"
        "#pragma once\n\n"
        '#include "lvgl.h"\n\n'
        "#define DINO_IMAGE_COUNT 8\n"
        "#define DINO_IMAGE_WIDTH 216\n"
        "#define DINO_IMAGE_HEIGHT 130\n\n"
        "/* T. rex, Triceratops, Stegosaurus, Brachiosaurus, Diplodocus,\n"
        " * Ankylosaurus, Spinosaurus, Velociraptor. */\n"
        "extern const lv_image_dsc_t dino_images[8];\n",
        encoding="utf-8",
    )
    (ASSETS / "checksums.json").write_text(json.dumps(checksums, indent=2) + "\n", encoding="utf-8")
    print("Converted 8 images: 216x130, little-endian RGB565, 56160 bytes each")
    print("Total image data: " + str(DATA_SIZE * len(entries)) + " bytes in Flash")


if __name__ == "__main__":
    main()
