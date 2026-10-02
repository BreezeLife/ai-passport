#!/usr/bin/env python3
"""Host tests for the configurable firmware-layout parser and verifier."""

from __future__ import annotations

import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_firmware", ROOT / "tools" / "verify_firmware.py"
)
assert SPEC and SPEC.loader
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


DEFAULT_TABLE_OFFSET = 0x8000
DEFAULT_APP_OFFSET = 0x10000
DEFAULT_APP_SIZE = VERIFY.FLASH_SIZE - DEFAULT_APP_OFFSET
DINO_ENTRIES = (
    (1, 2, 0x9000, 0x6000, "nvs"),
    (1, 1, 0xF000, 0x1000, "phy_init"),
    (0, 0, 0x10000, 0x300000, "factory"),
    (1, 2, 0x356000, 0x4000, "cardid"),
    (1, 0x40, 0x35A000, 0x3A6000, "dino_audio"),
    (0, 0x20, 0x700000, 0x100000, "recovery"),
)


def sample_table(entries: tuple[tuple[int, int, int, int, str], ...] | None = None) -> bytes:
    if entries is None:
        entries = (
            (1, 2, 0x9000, 0x6000, "nvs"),
            (1, 1, 0xF000, 0x1000, "phy_init"),
            (0, 0, DEFAULT_APP_OFFSET, DEFAULT_APP_SIZE, "factory"),
        )
    raw = bytearray(b"\xff" * VERIFY.PARTITION_TABLE_SIZE)
    for index, (kind, subtype, offset, size, label) in enumerate(entries):
        VERIFY.ENTRY.pack_into(
            raw,
            index * VERIFY.ENTRY.size,
            0x50AA,
            kind,
            subtype,
            offset,
            size,
            label.encode().ljust(16, b"\0"),
            0,
        )
    marker = len(entries) * VERIFY.ENTRY.size
    struct.pack_into("<H", raw, marker, 0xEBEB)
    raw[marker + 16 : marker + 32] = hashlib.md5(raw[:marker]).digest()
    return bytes(raw)


def merged_with_table(
    table: bytes | None = None,
    table_offset: int = DEFAULT_TABLE_OFFSET,
    app_offset: int = DEFAULT_APP_OFFSET,
) -> bytearray:
    merged = bytearray(b"\xff" * (max(table_offset + VERIFY.PARTITION_TABLE_SIZE, app_offset) + 1))
    merged[
        table_offset : table_offset + VERIFY.PARTITION_TABLE_SIZE
    ] = table or sample_table()
    merged[app_offset] = 0xE9
    return merged


class PartitionParserTest(unittest.TestCase):
    def test_parses_minimal_layout_and_md5(self) -> None:
        partitions, found_md5 = VERIFY.parse_partition_table(sample_table())
        self.assertTrue(found_md5)
        self.assertEqual([item.label for item in partitions], ["nvs", "phy_init", "factory"])
        self.assertEqual(partitions[-1].size, DEFAULT_APP_SIZE)

    def test_rejects_bad_md5(self) -> None:
        raw = bytearray(sample_table())
        raw[28] ^= 1
        with self.assertRaisesRegex(ValueError, "MD5"):
            VERIFY.parse_partition_table(bytes(raw))


class FirmwareLayoutTest(unittest.TestCase):
    def verify(
        self,
        merged: bytes,
        app_size: int = 1,
        table_offset: int = DEFAULT_TABLE_OFFSET,
        app_offset: int = DEFAULT_APP_OFFSET,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            with (build_dir / "FoloToy-AI-Passport.bin").open("wb") as app_file:
                app_file.write(b"\xe9")
                app_file.truncate(app_size)
            VERIFY.verify_firmware_layout(merged, build_dir, table_offset, app_offset)

    def test_layout_verification_accepts_current_partition_table(self) -> None:
        self.verify(bytes(merged_with_table()))

    def test_accepts_custom_data_partition(self) -> None:
        entries = (
            (1, 2, 0x9000, 0x6000, "nvs"),
            (1, 1, 0xF000, 0x1000, "phy_init"),
            (0, 0, DEFAULT_APP_OFFSET, 0x300000, "factory"),
            (1, 2, 0x310000, 0x4000, "unused"),
        )
        self.verify(bytes(merged_with_table(sample_table(entries))))

    def test_rejects_overlapping_partitions(self) -> None:
        entries = (
            (1, 2, 0x9000, 0x6000, "nvs"),
            (1, 1, 0xE000, 0x2000, "phy_init"),
            (0, 0, DEFAULT_APP_OFFSET, DEFAULT_APP_SIZE, "factory"),
        )
        with self.assertRaisesRegex(ValueError, "overlap"):
            self.verify(bytes(merged_with_table(sample_table(entries))))

    def test_accepts_moved_app_partition(self) -> None:
        app_offset = 0x20000
        entries = (
            (1, 2, 0x9000, 0x6000, "nvs"),
            (1, 1, 0xF000, 0x1000, "phy_init"),
            (0, 0, app_offset, VERIFY.FLASH_SIZE - app_offset, "factory"),
        )
        merged = merged_with_table(sample_table(entries), app_offset=app_offset)
        self.verify(bytes(merged), app_offset=app_offset)

    def test_rejects_oversized_application(self) -> None:
        with self.assertRaisesRegex(ValueError, "application .* partition limit"):
            self.verify(bytes(merged_with_table()), DEFAULT_APP_SIZE + 1)

    def test_rejects_missing_application_image(self) -> None:
        merged = merged_with_table()
        merged[DEFAULT_APP_OFFSET] = 0xFF
        with self.assertRaisesRegex(ValueError, "no ESP application image"):
            self.verify(bytes(merged))

    def test_rejects_app_offset_without_matching_partition(self) -> None:
        app_offset = 0x20000
        merged = merged_with_table(app_offset=app_offset)
        with self.assertRaisesRegex(ValueError, "match exactly one app partition"):
            self.verify(bytes(merged), app_offset=app_offset)

    def test_rejects_duplicate_partition_labels(self) -> None:
        entries = (
            (1, 2, 0x9000, 0x6000, "data"),
            (1, 1, 0xF000, 0x1000, "data"),
            (0, 0, DEFAULT_APP_OFFSET, DEFAULT_APP_SIZE, "factory"),
        )
        with self.assertRaisesRegex(ValueError, "labels must be unique"):
            self.verify(bytes(merged_with_table(sample_table(entries))))


class FlashArgsTest(unittest.TestCase):
    def test_parses_configured_image_offsets(self) -> None:
        offsets = VERIFY.parse_flash_args(
            "--flash_mode dio --flash_size 8MB\n"
            "0x0 bootloader/bootloader.bin\n"
            "0x18000 FoloToy-AI-Passport.bin\n"
            "0x9000 partition_table/partition-table.bin\n"
        )
        self.assertEqual(offsets["FoloToy-AI-Passport.bin"], 0x18000)
        self.assertEqual(offsets["partition_table/partition-table.bin"], 0x9000)

    def test_rejects_incomplete_image_entry(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid image entry"):
            VERIFY.parse_flash_args("0x20000\n")


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for the configure contract tests")
class AudioCmakeTest(unittest.TestCase):
    """Execute the root CMake staging/validation against small IDF API stubs.

    The complete firmware gate separately exercises the actual ESP-IDF APIs.
    These tests catch asset failures before an expensive firmware compilation.
    """
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="dinobook-cmake-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.source.joinpath("CMakeLists.txt").write_text(cmake.replace(
            "project(FoloToy-AI-Passport)", "project(FoloToy-AI-Passport LANGUAGES NONE)"
        ))
        self.audio = self.source / "assets/audio/dinobook40/audio.bin"
        self.audio.parent.mkdir(parents=True)
        self.idf = self.root / "idf"
        include = self.idf / "tools/cmake/project.cmake"
        include.parent.mkdir(parents=True)
        include.write_text('''
function(partition_table_get_partition_info output filter field)
  if(NOT "${filter}" STREQUAL "--partition-name dino_audio" OR NOT "${field}" STREQUAL "size")
    message(FATAL_ERROR "wrong partition query")
  endif()
  set(${output} 0x3a6000 PARENT_SCOPE)
endfunction()
function(esptool_py_flash_to_partition target partition image)
  if(NOT "${target}" STREQUAL "flash" OR NOT "${partition}" STREQUAL "dino_audio")
    message(FATAL_ERROR "wrong flash registration")
  endif()
  file(RELATIVE_PATH relative "${CMAKE_BINARY_DIR}" "${image}")
  file(WRITE "${CMAKE_BINARY_DIR}/flash_args" "0x35a000 ${relative}\\n")
endfunction()
''')

    def configure(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run([shutil.which("cmake"), "-S", str(self.source), "-B", str(self.root / "build")],
                              env={**os.environ, "IDF_PATH": str(self.idf)},
                              capture_output=True, text=True, check=False)

    def test_registers_and_refreshes_the_exact_staged_audio_bank(self) -> None:
        self.audio.write_bytes(b"first narration")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        staged = self.root / "build/dino_audio/audio.bin"
        self.assertEqual(staged.read_bytes(), self.audio.read_bytes())
        self.assertEqual((self.root / "build/flash_args").read_text(), "0x35a000 dino_audio/audio.bin\n")
        self.audio.write_bytes(b"revised narration")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(staged.read_bytes(), self.audio.read_bytes())

    def test_missing_bank_builds_a_public_source_profile_without_audio(self) -> None:
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Narration bank omitted", result.stderr)
        self.assertFalse((self.root / "build/dino_audio/audio.bin").exists())
        self.assertFalse((self.root / "build/flash_args").exists())

    def test_empty_bank_cannot_be_staged(self) -> None:
        self.audio.write_bytes(b"")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not be empty", result.stderr)
        self.assertFalse((self.root / "build/dino_audio/audio.bin").exists())

    def test_oversized_bank_cannot_be_staged(self) -> None:
        self.audio.write_bytes(b"x" * (0x3A6000 + 1))
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exceeds its", result.stderr)
        self.assertFalse((self.root / "build/dino_audio/audio.bin").exists())


class AudioStaticGateTest(unittest.TestCase):
    """Exercise the shell gate's resource profiles with a tiny verifier fixture."""
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="dinobook-audio-gate-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.audio = self.root / "assets/audio/dinobook40"
        self.audio.mkdir(parents=True)
        tools = self.root / "tools"
        tools.mkdir()
        # Execute the actual shell functions without the full compiler gate.
        script = (ROOT / "tools/validate.sh").read_text().split('\ncd "${repo_root}"\n', 1)[0]
        self.script = tools / "validate_audio_test.sh"
        self.script.write_text(script + "\nrun_dino_audio_checks\n")
        (tools / "generate_dino_audio.py").write_text(
            "from pathlib import Path\n"
            "assert Path('assets/audio/dinobook40/audio.bin').read_bytes() == b'valid bank'\n"
            "assert Path('assets/audio/dinobook40/000_name.wav').read_bytes() == b'valid wav'\n"
            "print('Audio verifier executed')\n"
        )

    def run_gate(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(self.script)], cwd=self.root,
                              capture_output=True, text=True, check=False)

    def test_public_source_profile_reports_audio_checks_not_run(self) -> None:
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Audio asset checks: NOT RUN", result.stdout)
        self.assertNotIn("Audio asset checks: PASS", result.stdout)
        self.assertNotIn("Audio verifier executed", result.stdout)

    def test_partial_audio_resources_are_rejected_instead_of_skipped(self) -> None:
        (self.audio / "000_name.wav").write_bytes(b"valid wav")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Incomplete DinoBook narration resources", result.stderr)
        self.assertNotIn("Audio asset checks: NOT RUN", result.stdout)

    def test_present_bank_runs_the_audio_verifier(self) -> None:
        (self.audio / "audio.bin").write_bytes(b"valid bank")
        (self.audio / "000_name.wav").write_bytes(b"valid wav")
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Audio verifier executed", result.stdout)
        self.assertIn("Audio asset checks: PASS", result.stdout)

    def test_corrupt_bank_is_not_misreported_as_an_omitted_resource(self) -> None:
        (self.audio / "audio.bin").write_bytes(b"corrupt bank")
        (self.audio / "000_name.wav").write_bytes(b"valid wav")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Audio asset checks: NOT RUN", result.stdout)
        self.assertNotIn("Audio asset checks: PASS", result.stdout)


class FirmwareCliTest(unittest.TestCase):
    CUSTOM_ENTRIES = (
        (1, 2, 0x9000, 0x6000, "nvs"),
        (1, 1, 0xF000, 0x1000, "phy_init"),
        (0, 0, 0x10000, 0x10000, "factory"),
        (1, 0x82, 0x20000, 0x1000, "assets"),
        (1, 0x40, 0x21000, 0x1000, "metadata"),
    )

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.build_dir = Path(self.directory.name)

    def create_build(
        self,
        extras: tuple[tuple[str, int, bytes], ...] = (),
        entries: tuple[tuple[int, int, int, int, str], ...] | None = None,
        app_offset: int = DEFAULT_APP_OFFSET,
    ) -> None:
        images = (
            ("bootloader/bootloader.bin", 0, b"\xe9boot"),
            ("partition_table/partition-table.bin", DEFAULT_TABLE_OFFSET, sample_table(entries)),
            ("FoloToy-AI-Passport.bin", app_offset, b"\xe9app"),
        ) + extras
        merged = bytearray()
        flash_args = ["--flash_mode dio --flash_size 8MB"]
        for name, offset, data in images:
            image_path = self.build_dir / name
            image_path.parent.mkdir(parents=True, exist_ok=True)
            image_path.write_bytes(data)
            flash_args.append(f'{offset:#x} "{name}"')
            # Invalid addresses still reach the CLI without allocating beyond
            # the physical Flash size in the test fixture.
            if 0 <= offset < VERIFY.FLASH_SIZE:
                end = min(offset + len(data), VERIFY.FLASH_SIZE)
                merged.extend(b"\xff" * max(0, end - len(merged)))
                merged[offset:end] = data[:end - offset]
        (self.build_dir / "FoloToy-AI-Passport-full.bin").write_bytes(merged)
        (self.build_dir / "flash_args").write_text("\n".join(flash_args) + "\n")

    def run_verifier(self, error: str | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/verify_firmware.py"), str(self.build_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        if error is None:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Merged firmware: PASS", result.stdout)
        else:
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(error, result.stderr)
            self.assertNotIn("Merged firmware: PASS", result.stdout)
            self.assertNotIn("Traceback", result.stderr)
        return result

    def test_dinobook_audio_and_identity_flash_impact_are_verified(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"audio"),), DINO_ENTRIES)
        result = self.run_verifier()
        self.assertIn("Flashing this merged image erases identity data", result.stdout)
        self.assertIn("Recovery contents excluded", result.stdout)
        full = (self.build_dir / "FoloToy-AI-Passport-full.bin").read_bytes()
        self.assertEqual(full[0x356000:0x35A000], b"\xff" * 0x4000)
        self.assertLess(len(full), 0x700000)

    def test_dinobook_public_source_profile_accepts_an_omitted_audio_image(self) -> None:
        self.create_build(entries=DINO_ENTRIES)
        result = self.run_verifier()
        self.assertIn("DinoBook audio: NOT INCLUDED", result.stdout)
        self.assertNotIn("DinoBook audio: PASS", result.stdout)
        self.assertIn("ends before cardid", result.stdout)

    def test_dinobook_silent_profile_still_requires_the_fixed_partition_layout(self) -> None:
        entries = tuple(
            (kind, subtype, 0x355000 if label == "cardid" else offset, size, label)
            for kind, subtype, offset, size, label in DINO_ENTRIES
        )
        self.create_build(entries=entries)
        self.run_verifier("partition 'cardid' must retain its fixed layout")

    def test_dinobook_silent_profile_rejects_protected_partition_payloads(self) -> None:
        for label, offset in (("cardid", 0x356000), ("recovery", 0x700000)):
            with self.subTest(label=label):
                self.create_build(((f"{label}.bin", offset, b"payload"),), DINO_ENTRIES)
                self.run_verifier(f"must not write the {label} partition")

    def test_dinobook_rejects_an_audio_image_registered_under_another_name(self) -> None:
        self.create_build((("unexpected-audio.bin", 0x35A000, b"audio"),), DINO_ENTRIES)
        self.run_verifier("requires dino_audio/audio.bin at its partition start")

    def test_dinobook_registered_audio_cannot_be_empty(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b""),), DINO_ENTRIES)
        self.run_verifier("dino_audio/audio.bin is empty")

    def test_dinobook_silent_profile_still_rejects_non_ff_identity_padding(self) -> None:
        self.create_build(entries=DINO_ENTRIES)
        path = self.build_dir / "FoloToy-AI-Passport-full.bin"
        merged = bytearray(path.read_bytes().ljust(0x356001, b"\xff"))
        merged[0x356000] = ord("X")
        path.write_bytes(merged)
        self.run_verifier("identity hole must contain only FF padding")

    def test_dinobook_silent_profile_still_excludes_recovery_contents(self) -> None:
        self.create_build(entries=DINO_ENTRIES)
        path = self.build_dir / "FoloToy-AI-Passport-full.bin"
        path.write_bytes(path.read_bytes().ljust(0x700001, b"\xff"))
        self.run_verifier("must end before Recovery contents")

    def test_dinobook_audio_must_start_at_the_partition_offset(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A010, b"audio"),), DINO_ENTRIES)
        self.run_verifier("requires dino_audio/audio.bin at its partition start")

    def test_dinobook_audio_cannot_overflow_into_recovery(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"a" * (0x3A6000 + 1)),), DINO_ENTRIES)
        self.run_verifier("must fit entirely within one partition")

    def test_dinobook_audio_can_fill_its_partition_without_touching_recovery(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"a" * 0x3A6000),), DINO_ENTRIES)
        self.run_verifier()
        self.assertEqual((self.build_dir / "FoloToy-AI-Passport-full.bin").stat().st_size, 0x700000)

    def test_dinobook_partition_contract_cannot_drift(self) -> None:
        entries = tuple(
            (kind, subtype, 0x355000 if label == "cardid" else offset, size, label)
            for kind, subtype, offset, size, label in DINO_ENTRIES
        )
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"audio"),), entries)
        self.run_verifier("partition 'cardid' must retain its fixed layout")

    def test_dinobook_rejects_separate_identity_and_recovery_payloads(self) -> None:
        for label, offset in (("cardid", 0x356000), ("recovery", 0x700000)):
            with self.subTest(label=label):
                self.create_build((
                    ("dino_audio/audio.bin", 0x35A000, b"audio"),
                    (f"{label}.bin", offset, b"payload"),
                ), DINO_ENTRIES)
                self.run_verifier(f"must not write the {label} partition")

    def test_dinobook_identity_hole_must_be_ff_padding(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"audio"),), DINO_ENTRIES)
        with (self.build_dir / "FoloToy-AI-Passport-full.bin").open("r+b") as full:
            full.seek(0x356000)
            full.write(b"X")
        self.run_verifier("identity hole must contain only FF padding")

    def test_dinobook_merged_tail_cannot_include_recovery_padding(self) -> None:
        self.create_build((("dino_audio/audio.bin", 0x35A000, b"audio"),), DINO_ENTRIES)
        path = self.build_dir / "FoloToy-AI-Passport-full.bin"
        path.write_bytes(path.read_bytes().ljust(0x700001, b"\xff"))
        self.run_verifier("must end before Recovery contents")

    def test_accepts_default_minimal_layout(self) -> None:
        self.create_build()
        self.run_verifier()

    def test_accepts_additional_resources_at_partition_start_or_inside(self) -> None:
        self.create_build(
            extras=(
                ("assets.bin", 0x20000, b"resource"),
                ("metadata chunk.bin", 0x21010, b"metadata"),
            ),
            entries=self.CUSTOM_ENTRIES,
        )
        self.run_verifier()

    def test_accepts_custom_ota_layout_and_images(self) -> None:
        entries = (
            (1, 2, 0x9000, 0x4000, "settings"),
            (1, 0, 0xD000, 0x2000, "otadata"),
            (1, 1, 0xF000, 0x1000, "phy"),
            (0, 0x10, 0x10000, 0x30000, "ota_0"),
            (0, 0x11, 0x40000, 0x30000, "ota_1"),
            (1, 0x82, 0x70000, 0x2000, "resources"),
        )
        self.create_build(
            extras=(
                ("ota_data_initial.bin", 0xD000, b"\xff" * 0x2000),
                ("second-app.bin", 0x40000, b"\xe9other-app"),
                ("resources.bin", 0x70000, b"resource"),
            ),
            entries=entries,
        )
        self.run_verifier()

    def test_accepts_moved_application(self) -> None:
        self.create_build(
            entries=((0, 0, 0x20000, VERIFY.FLASH_SIZE - 0x20000, "application"),),
            app_offset=0x20000,
        )
        self.run_verifier()

    def test_rejects_missing_resource_file(self) -> None:
        self.create_build((("assets.bin", 0x20000, b"resource"),), self.CUSTOM_ENTRIES)
        (self.build_dir / "assets.bin").unlink()
        self.run_verifier("missing image")

    def test_rejects_empty_resource_file(self) -> None:
        self.create_build((("assets.bin", 0x20000, b""),), self.CUSTOM_ENTRIES)
        self.run_verifier("assets.bin is empty")

    def test_rejects_resource_missing_from_merged_image(self) -> None:
        self.create_build((("assets.bin", 0x20000, b"resource"),), self.CUSTOM_ENTRIES)
        with (self.build_dir / "FoloToy-AI-Passport-full.bin").open("r+b") as merged:
            merged.truncate(0x20000)
        self.run_verifier("assets.bin differs")

    def test_rejects_resource_byte_mismatch(self) -> None:
        self.create_build((("assets.bin", 0x20000, b"resource"),), self.CUSTOM_ENTRIES)
        with (self.build_dir / "FoloToy-AI-Passport-full.bin").open("r+b") as merged:
            merged.seek(0x20000)
            merged.write(b"X")
        self.run_verifier("assets.bin differs")

    def test_rejects_negative_image_offset(self) -> None:
        self.create_build((("assets.bin", -1, b"resource"),), self.CUSTOM_ENTRIES)
        self.run_verifier("outside the 8 MB flash bounds")

    def test_rejects_image_extending_past_flash(self) -> None:
        self.create_build((("assets.bin", VERIFY.FLASH_SIZE - 1, b"resource"),))
        self.run_verifier("outside the 8 MB flash bounds")

    def test_rejects_image_starting_outside_flash(self) -> None:
        self.create_build((("assets.bin", VERIFY.FLASH_SIZE, b"resource"),))
        self.run_verifier("outside the 8 MB flash bounds")

    def test_rejects_overlapping_images(self) -> None:
        self.create_build(
            (("assets.bin", 0x20000, b"resource"), ("other.bin", 0x20004, b"overlap")),
            self.CUSTOM_ENTRIES,
        )
        self.run_verifier("images 'assets.bin' and 'other.bin' overlap")

    def test_rejects_resource_overlapping_required_image(self) -> None:
        self.create_build((("assets.bin", DEFAULT_APP_OFFSET, b"\xe9app"),))
        self.run_verifier("overlap")

    def test_rejects_resource_crossing_adjacent_partitions(self) -> None:
        self.create_build((("assets.bin", 0x20FFF, b"resource"),), self.CUSTOM_ENTRIES)
        self.run_verifier("assets.bin must fit entirely within one partition")

    def test_rejects_resource_in_unpartitioned_gap(self) -> None:
        self.create_build((("assets.bin", 0x23000, b"resource"),), self.CUSTOM_ENTRIES)
        self.run_verifier("assets.bin must fit entirely within one partition")

    def test_still_requires_all_boot_images(self) -> None:
        self.create_build()
        args_path = self.build_dir / "flash_args"
        args = args_path.read_text()
        args_path.write_text("\n".join(line for line in args.splitlines() if "bootloader/" not in line))
        self.run_verifier("missing required images")

    def test_rejects_empty_required_image(self) -> None:
        self.create_build()
        (self.build_dir / "bootloader/bootloader.bin").write_bytes(b"")
        self.run_verifier("bootloader/bootloader.bin is empty")

    def test_rejects_malformed_extra_image_entry(self) -> None:
        self.create_build()
        with (self.build_dir / "flash_args").open("a") as args:
            args.write("0x20000 assets.bin unexpected-token\n")
        self.run_verifier("invalid image entry")

    def test_rejects_unparseable_extra_image_offset(self) -> None:
        self.create_build()
        with (self.build_dir / "flash_args").open("a") as args:
            args.write("0x2000g assets.bin\n")
        self.run_verifier("invalid image offset")


if __name__ == "__main__":
    unittest.main()
