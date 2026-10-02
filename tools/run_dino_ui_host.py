#!/usr/bin/env python3
"""Render the real application with a local LVGL 9.5.0 source tree.

Example: python3 tools/run_dino_ui_host.py --lvgl-source /path/to/lvgl
Requires a host C/C++ compiler, CMake and Pillow; never downloads dependencies.
This optional check does not change the ESP-IDF or static validation gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, log: Path) -> None:
    with log.open("w") as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        print("\n".join(log.read_text().splitlines()[-60:]))
        raise SystemExit(result.returncode)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-source", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path, default=Path("/tmp/dinobook-ui-host"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_files = [
        "main/dino_ui.c", "main/dino_ui.h", "main/dino_model.c", "main/dino_model.h",
        "main/dino_catalog.c", "main/dino_catalog.h", "main/dino_animation.c",
        "main/dino_animation.h", "main/dino_animation_assets.c",
        "assets/fonts/dino_font_14.c", "assets/fonts/dino_font_18.c", "assets/fonts/dino_font_24.c",
        "tests/dino_ui_host/render.c", "tests/dino_ui_host/lv_conf.h",
        "tests/dino_ui_host/CMakeLists.txt",
        "tools/run_dino_ui_host.py", "tools/generate_dino_animation.py",
        "assets/animations/dinosaurs40/generation-plan.json",
        "assets/animations/dinosaurs40/metadata.json",
    ]
    def file_hash(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    missing = [name for name in source_files if not (root / name).is_file()]
    if missing:
        parser.error("complete native assets are required; missing: " + ", ".join(missing))
    source_hashes = {name: file_hash(root / name) for name in source_files}
    source = args.lvgl_source.expanduser().resolve()
    if not (source / "lvgl.h").is_file():
        parser.error("--lvgl-source must contain an existing LVGL source tree")
    build = args.build_dir.expanduser().resolve()
    preview = (args.output_dir or root / "build/preview").expanduser().resolve()
    build.mkdir(parents=True, exist_ok=True)
    # A fresh inventory prevents screenshots from an older UI/species set from
    # silently appearing in the current contact sheet.
    frames = Path(tempfile.mkdtemp(prefix="frames-", dir=build))
    preview.mkdir(parents=True, exist_ok=True)
    run(["cmake", "-S", str(root / "tests/dino_ui_host"), "-B", str(build),
         f"-DLVGL_SOURCE={source}", "-DCMAKE_BUILD_TYPE=Release"], build / "configure.log")
    run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], build / "build.log")
    result = subprocess.run([str(build / "dino_ui_host"), str(frames)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (preview / "dino-ui-host-results.txt").write_text(result.stdout)
    print(result.stdout, end="")
    result_code = result.returncode
    if source_hashes != {name: file_hash(root / name) for name in source_files}:
        print("Host source changed while compiling/rendering; rerun to bind evidence to stable files.")
        result_code = 1
    manifest = {
        "format_version": 1, "host_tests": "PASS" if result_code == 0 else "FAIL",
        "engine": "LVGL 9.5.0", "device_photographs": False, "pool_bytes": 49152,
        "source_sha256": source_hashes,
        "report": {"path": "dino-ui-host-results.txt",
                   "sha256": file_hash(preview / "dino-ui-host-results.txt")},
        "gif_durations_ms": [120, 130] * 4, "species": [],
    }
    metadata_path = root / "assets/animations/dinosaurs40/metadata.json"
    if metadata_path.is_file():
        manifest["assets_metadata_sha256"] = file_hash(metadata_path)

    from PIL import Image, ImageDraw
    images = sorted(frames.glob("*.ppm"))
    if images:
        columns, gap, caption = 8, 12, 24
        rows = (len(images) + columns - 1) // columns
        sheet = Image.new("RGB", (columns * (240 + gap) + gap,
                                  rows * (320 + caption + gap) + gap + 36), "#e7ebdd")
        painter = ImageDraw.Draw(sheet)
        painter.text((gap, 10), "Dino Passport / real LVGL 9.5.0 host renders / not device photographs", fill="#244d3c")
        for index, path in enumerate(images):
            x, y = gap + (index % columns) * (240 + gap), 36 + gap + (index // columns) * (320 + caption + gap)
            with Image.open(path) as image:
                image.save(preview / (path.stem + ".png"))
                sheet.paste(image, (x, y))
            painter.text((x, y + 324), path.stem[:37], fill="#244d3c")
        destination = preview / "dino-ui-contact-sheet.png"
        sheet.save(destination)
        manifest["contact_sheet"] = {"path": destination.name, "sha256": file_hash(destination)}
        print(f"Host contact sheet: {destination} ({len(images)} frames)")
        motion_first = sorted(frames.glob("08-dino-*-page-8-variant-0.ppm"),
                              key=lambda path: int(path.stem.split("-")[2]))
        if len(motion_first) == 40:
            plan = json.loads((root / "assets/animations/dinosaurs40/generation-plan.json").read_text())
            overview = Image.new("RGB", (columns * (240 + gap) + gap,
                                         5 * (320 + caption + gap) + gap + 36), "#e7ebdd")
            overview_draw = ImageDraw.Draw(overview)
            overview_draw.text((gap, 10), "40 dinosaur motion pages / real LVGL host / not device photographs", fill="#244d3c")
            for index, path in enumerate(motion_first):
                x = gap + (index % columns) * (240 + gap)
                y = 36 + gap + (index // columns) * (320 + caption + gap)
                with Image.open(path) as image:
                    overview.paste(image, (x, y))
                overview_draw.text((x, y + 324), f"Species {index + 1}", fill="#244d3c")
                sequence = []
                for frame in range(8):
                    frame_path = frames / f"08-dino-{index + 1}-page-8-variant-{frame}.ppm"
                    with Image.open(frame_path) as image:
                        sequence.append(image.convert("RGB"))
                gif_path = preview / f"dino-motion-host-{index + 1:02}.gif"
                sequence[0].save(gif_path, save_all=True,
                                 append_images=sequence[1:], duration=[120, 130] * 4, loop=0)
                frame_names = [f"08-dino-{index + 1}-page-8-variant-{frame}.png" for frame in range(8)]
                manifest["species"].append({
                    "index": index, "id": plan["species"][index]["id"],
                    "first_frame": frame_names[0], "gif": gif_path.name, "frames": frame_names,
                    "first_frame_sha256": file_hash(preview / frame_names[0]),
                    "gif_sha256": file_hash(gif_path),
                    "frame_sha256": [file_hash(preview / name) for name in frame_names],
                })
            overview_path = preview / "dino-motion-species-sheet.png"
            overview.save(overview_path)
            manifest["motion_overview"] = {"path": overview_path.name, "sha256": file_hash(overview_path)}
            print(f"Motion overview: {overview_path}; 40 real-LVGL GIF previews")
    if len(manifest["species"]) != 40:
        manifest["host_tests"] = "FAIL"
        result_code = result_code or 1
    (preview / "dino-ui-host-preview.json").write_text(json.dumps(manifest, indent=2) + "\n")
    raise SystemExit(result_code)


if __name__ == "__main__":
    main()
