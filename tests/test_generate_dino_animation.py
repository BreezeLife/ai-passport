"""Animation conversion checks using synthetic atlases in temporary folders."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dino_animation_generator", ROOT / "tools/generate_dino_animation.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def fixture(directory, count=40):
    directory.mkdir()
    (directory / "originals").mkdir()
    plan = {"frames": 8, "grid": [3, 3], "width": 144, "height": 88,
            "requested_model": "synthetic test fixture",
            "observed_model_id": "synthetic test fixture",
            "species": [{"id": f"species{i}"} for i in range(40)]}
    (directory / "generation-plan.json").write_text(json.dumps(plan))
    atlas = Image.new("RGB", (91, 55), GEN.PAPER)
    draw = ImageDraw.Draw(atlas)
    for frame in range(8):
        row, col = divmod(frame, 3)
        x, y = col * 91 // 3, row * 55 // 3
        draw.rectangle((x + 1 + frame, y + 2, x + 5 + frame, y + 14),
                       fill=(40 + frame * 20, 90, 130))
        draw.point((x, y), fill=(255, 0, 0))
    atlas.paste(atlas.crop((0, 0, 30, 18)), (60, 36))  # Ignored ninth repeats first.
    for index in range(count):
        atlas.save(directory / f"originals/species{index}.png")
    return plan


class ConverterTest(unittest.TestCase):
    def test_missing_complete_and_partial_preserves_firmware(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "assets"
            fixture(directory, 1)
            c_output = Path(temporary) / "assets.c"
            c_output.write_bytes(b"existing firmware must remain untouched")
            with self.assertRaisesRegex(GEN.AssetError, "missing 39/40"):
                GEN.generate(directory, c_output)
            self.assertFalse((directory / "metadata.json").exists())
            partial = GEN.generate(directory, c_output, partial=True)
            self.assertFalse(partial["complete"])
            self.assertEqual(partial["species_count"], 1)
            self.assertEqual(len(partial["missing_originals"]), 39)
            self.assertEqual(c_output.read_bytes(), b"existing firmware must remain untouched")
            self.assertFalse((directory / "converted").exists())
            with self.assertRaises(OSError):
                GEN.verify(directory, c_output)

    def test_full_cells_letterbox_nibbles_and_shared_palette(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "assets"
            fixture(directory, 1)
            converted = GEN.convert_original(directory / "originals/species0.png")
            self.assertEqual(converted["crop_boxes"][0], [0, 0, 30, 18])
            self.assertEqual(converted["crop_boxes"][2], [60, 0, 91, 18])
            self.assertEqual(converted["crop_boxes"][7], [30, 36, 60, 55])
            self.assertEqual(len(converted["palette"]), 16)
            self.assertEqual(len(converted["packed"]), 8 * 6336)
            self.assertEqual(len(set(converted["raw_frames"])), 8)
            self.assertTrue(all(value > 0 for value in converted["changed_pixels"]))
            self.assertEqual(GEN.unpack_rgb565(bytes([0x21, 0xfe]), list(range(16))),
                             b"\x01\x00\x02\x00\x0e\x00\x0f\x00")

    def test_duplicate_source_cells_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            image = Path(temporary) / "duplicate.png"
            Image.new("RGB", (90, 54), GEN.PAPER).save(image)
            with self.assertRaisesRegex(GEN.AssetError, "not unique"):
                GEN.convert_original(image)

    def test_strict_plan(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "assets"
            plan = fixture(directory, 0)
            for alteration in (lambda p: p.update(width=145),
                               lambda p: p.update(frames=9),
                               lambda p: p["species"].pop(),
                               lambda p: p["species"][0].update(id="../outside"),
                               lambda p: p["species"][0].update(id="species1")):
                modified = json.loads(json.dumps(plan))
                alteration(modified)
                (directory / "generation-plan.json").write_text(json.dumps(modified))
                with self.assertRaises(GEN.AssetError):
                    GEN.read_plan(directory)

    def test_complete_real_decoder_readonly_and_tampering(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "assets"
            fixture(directory)
            c_output = Path(temporary) / "dino_animation_assets.c"
            metadata = GEN.generate(directory, c_output)
            self.assertEqual(metadata["payload_bytes"], 2028800)
            self.assertEqual(metadata["species_count"], 40)
            snapshots = {path: (path.stat().st_mtime_ns, GEN.digest(path.read_bytes()))
                         for path in Path(temporary).rglob("*") if path.is_file()}
            verified = GEN.verify(directory, c_output)
            self.assertTrue(verified["complete"])
            self.assertEqual(snapshots, {path: (path.stat().st_mtime_ns, GEN.digest(path.read_bytes()))
                                         for path in Path(temporary).rglob("*") if path.is_file()})
            first = metadata["species"][0]
            paths = [directory / first[key]["path"] for key in ("source", "palette", "packed", "gif")]
            paths += [directory / first["frames"][0]["preview"], c_output]
            for path in paths:
                original = path.read_bytes()
                corrupt = bytearray(original)
                corrupt[len(corrupt) // 2] ^= 1
                path.write_bytes(corrupt)
                with self.assertRaisesRegex(GEN.AssetError, "hash mismatch"):
                    GEN.verify(directory, c_output)
                path.write_bytes(original)
            metadata_path = directory / "metadata.json"
            original = metadata_path.read_bytes()
            for alteration in (lambda m: m.update(complete=False),
                               lambda m: m.update(frame_interval_ms=124),
                               lambda m: m["species"][0].update(index=1),
                               lambda m: m["species"][0].update(unique_frames=7),
                               lambda m: m["species"][0]["frames"][0].update(rgb565_sha256="bad"),
                               lambda m: m["species"][0]["gif"].update(loop_ms=999)):
                modified = json.loads(original)
                alteration(modified)
                metadata_path.write_text(json.dumps(modified))
                with self.assertRaises(GEN.AssetError):
                    GEN.verify(directory, c_output)
            metadata_path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
