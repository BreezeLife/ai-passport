#!/usr/bin/env python3
"""Generate/verify offline Mandarin development narration and IMA ADPCM assets.

Generation requires macOS say/afconvert. Verification uses Python 3.9+ standard
library and a host C compiler; audioop is not required. No speaker playback.
"""

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import wave

from generate_dino_catalog import validate as validate_catalog


ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "assets/data/dinosaurs.json"
OUTPUT = ROOT / "assets/audio/dinobook40"
LEGACY = ROOT / "assets/audio/dinobook"
CLIP_COUNT = 202
BLOB_HEADER_BYTES = 64
MAX_BLOB_BYTES = 3_700_000
BLOB_MAGIC = b"DINOA40\x00"
SAMPLE_RATE = 8000
VOICE = "Tingting"
RATE = 155
PEAK = 12000
DISCOVERY_TEXT = "发现啦！给你留下一枚恐龙足迹。"
WELCOME_TEXT = "欢迎来到小小恐龙护照。一起认识恐龙，收集恐龙足迹吧！"

IMA_STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
)
IMA_INDEX_CHANGES = (-1, -1, -1, -1, 2, 4, 6, 8)


def reference_decode(data):
    predictor, index = 0, 0
    samples = []
    for byte in data:
        for code in (byte & 15, byte >> 4):
            step = IMA_STEPS[index]
            change = (step // 8 + (step if code & 4 else 0) +
                      (step // 2 if code & 2 else 0) +
                      (step // 4 if code & 1 else 0))
            predictor = max(-32768, min(32767, predictor + (-change if code & 8 else change)))
            index = max(0, min(88, index + IMA_INDEX_CHANGES[code & 7]))
            samples.append(predictor)
    return struct.pack("<" + "h" * len(samples), *samples), (predictor, index)


def reference_encode(pcm):
    samples = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
    if len(samples) % 2:
        samples += (0,)
    predictor, index = 0, 0
    encoded = bytearray()
    low_code = 0
    for position, sample in enumerate(samples):
        step = IMA_STEPS[index]
        difference = sample - predictor
        code = 8 if difference < 0 else 0
        difference = abs(difference)
        change = step // 8
        for flag, part in ((4, step), (2, step // 2), (1, step // 4)):
            if difference >= part:
                code |= flag
                difference -= part
                change += part
        predictor = max(-32768, min(32767, predictor + (-change if code & 8 else change)))
        index = max(0, min(88, index + IMA_INDEX_CHANGES[code & 7]))
        if position % 2:
            encoded.append(low_code | (code << 4))
        else:
            low_code = code
    return bytes(encoded)


def pcm_peak(pcm):
    return max((abs(value[0]) for value in struct.iter_unpack("<h", pcm)), default=0)


def scale_pcm(pcm, factor):
    samples = [max(-32768, min(32767, math.floor(value[0] * factor)))
               for value in struct.iter_unpack("<h", pcm)]
    return struct.pack("<" + "h" * len(samples), *samples)


def verify_reference_vectors():
    # Golden results produced by Python 3.9 audioop before its 3.13 removal.
    expected = (0, 1, 4, 8, 15, 27, 47, 88, 82, 66, 41, 10, -28, -84, -181, -380)
    pcm, state = reference_decode(bytes.fromhex("1032547698badcfe"))
    if pcm != struct.pack("<16h", *expected) or state != (-380, 36):
        raise ValueError("Independent IMA reference failed the known decode vector")
    if reference_encode(pcm) != bytes.fromhex("1032547698badcfe"):
        raise ValueError("Independent IMA reference failed the known encode vector")
    saturated = struct.pack("<8h", -32768, -12000, -1, 0, 1, 12000, 32767, 0)
    if reference_encode(saturated) != bytes.fromhex("ff0478d7"):
        raise ValueError("Independent IMA reference failed the signed encode vector")
    if reference_encode(struct.pack("<3h", -100, 0, 100)) != bytes.fromhex("2fb7"):
        raise ValueError("Independent IMA reference failed the odd sample vector")



class DecoderState(ctypes.Structure):
    _fields_ = [("predictor", ctypes.c_int), ("index", ctypes.c_int)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def narration(catalog):
    validate_catalog(catalog)
    clips = []
    for i, dinosaur in enumerate(catalog):
        clips.append({"id": i, "role": "name", "dinosaur": dinosaur["id"],
                      "text": dinosaur["name"]})
        for fact in range(2):
            clips.append({"id": 40 + i * 2 + fact, "role": "fact",
                          "dinosaur": dinosaur["id"], "fact": fact,
                          "text": dinosaur["spoken_facts"][fact]})
        options = dinosaur["options"]
        clips.append({"id": 120 + i, "role": "question", "dinosaur": dinosaur["id"],
                      "text": dinosaur["question"] + "一，" + options[0] + "。二，" + options[1] + "。"})
        clips.append({"id": 160 + i, "role": "hint", "dinosaur": dinosaur["id"],
                      "text": dinosaur["hint"]})
    clips.extend([{"id": 200, "role": "discovery", "text": DISCOVERY_TEXT},
                  {"id": 201, "role": "welcome", "text": WELCOME_TEXT}])
    clips.sort(key=lambda clip: clip["id"])
    if [clip["id"] for clip in clips] != list(range(CLIP_COUNT)):
        raise ValueError("Unexpected narration ID layout")
    return clips


def compile_decoder(directory):
    library_path = directory / "dino_decoder.so"
    shared_flag = "-dynamiclib" if sys.platform == "darwin" else "-shared"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIC", shared_flag,
                    "-I", str(ROOT / "main"), str(ROOT / "main/dino_adpcm.c"),
                    "-o", str(library_path)], check=True)
    library = ctypes.CDLL(str(library_path))
    library.dino_adpcm_init.argtypes = [ctypes.POINTER(DecoderState), ctypes.c_int,
                                        ctypes.c_int]
    library.dino_adpcm_init.restype = ctypes.c_bool
    library.dino_adpcm_decode.argtypes = [ctypes.POINTER(DecoderState),
                                          ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                          ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
    library.dino_adpcm_decode.restype = ctypes.c_size_t
    return library


def encode(pcm):
    # Complete the last byte with an encoded zero sample, not a bare zero nibble.
    return reference_encode(pcm)


def verify_decoder(library, data):
    reference, final_state = reference_decode(data)
    state = DecoderState()
    if not library.dino_adpcm_init(ctypes.byref(state), 0, 0):
        raise ValueError("C decoder refused the initial state")
    codes = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    samples = (ctypes.c_int16 * (len(data) * 2))()
    count = library.dino_adpcm_decode(ctypes.byref(state), codes, len(data),
                                      samples, len(samples))
    # Explicit little endian packing makes the comparison independent of host order.
    decoded = struct.pack("<" + "h" * count, *samples[:count])
    if count != len(data) * 2 or decoded != reference:
        raise ValueError("C decoder disagrees with the standard-library IMA reference")
    if (state.predictor, state.index) != final_state:
        raise ValueError("C decoder final state disagrees with the reference")
    return decoded


def read_wav(path):
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate(),
            source.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
            raise ValueError("Unexpected WAV format: " + str(path))
        return source.readframes(source.getnframes())


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(SAMPLE_RATE)
        destination.writeframes(pcm)


def normalize_and_trim(pcm):
    peak = pcm_peak(pcm)
    if not peak:
        raise ValueError("Speech synthesis returned silence")
    normalized = scale_pcm(pcm, PEAK / peak)
    samples = struct.unpack("<" + "h" * (len(normalized) // 2), normalized)
    active = [i for i, sample in enumerate(samples) if abs(sample) > 100]
    if not active:
        raise ValueError("No speech detected after normalization")
    # Preserve a 20 ms margin around low-level consonants, then add short silence.
    margin = SAMPLE_RATE // 50
    start = max(0, active[0] - margin)
    end = min(len(samples), active[-1] + margin + 1)
    return (b"\x00\x00" * (SAMPLE_RATE // 10) + normalized[start * 2:end * 2] +
            b"\x00\x00" * (SAMPLE_RATE * 18 // 100))


def synthesize(clip, scratch, output):
    text_path = scratch / ("speech_%03d.txt" % clip["id"])
    aiff_path = scratch / ("speech_%03d.aiff" % clip["id"])
    wave_path = scratch / ("speech_%03d.wav" % clip["id"])
    text_path.write_text(clip["text"], encoding="utf-8")
    for attempt in range(3):
        try:
            subprocess.run(["say", "-v", VOICE, "-r", str(RATE), "-f", str(text_path),
                            "-o", str(aiff_path)], check=True, timeout=30)
            break
        except subprocess.TimeoutExpired:
            if attempt == 2:
                raise
            print("Retrying stalled offline speech clip %03d" % clip["id"], flush=True)
    subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@%d" % SAMPLE_RATE, "-c", "1",
                    str(aiff_path), str(wave_path)], check=True)
    pcm = normalize_and_trim(read_wav(wave_path))
    write_wav(output, pcm)
    return pcm


def blob_header(payload):
    # Firmware compares all 64 bytes before playback; it does not hash at runtime.
    return (BLOB_MAGIC + struct.pack("<I", BLOB_HEADER_BYTES + len(payload)) +
            hashlib.sha256(payload).digest() + bytes(20))


def emit_header(blob_size):
    return ("#pragma once\n\n#include <stdint.h>\n\n"
            "#define DINO_AUDIO_CLIP_COUNT 202\n"
            "#define DINO_AUDIO_SAMPLE_RATE 8000\n"
            "#define DINO_AUDIO_BLOB_HEADER_BYTES 64\n"
            "#define DINO_AUDIO_BLOB_SIZE %dU\n\n"
            "typedef struct {\n    uint32_t offset;\n    uint32_t bytes;\n    uint32_t samples;\n} dino_audio_clip_t;\n\n"
            "/* Absolute blob offsets include the 64-byte identity header.\n"
            " * Every clip starts with predictor/index zero; samples excludes padding. */\n"
            "extern const uint8_t dino_audio_blob_header[DINO_AUDIO_BLOB_HEADER_BYTES];\n"
            "extern const dino_audio_clip_t dino_audio_clips[DINO_AUDIO_CLIP_COUNT];\n" % blob_size).encode("utf-8")


def emit_c(clips, header):
    lines = ['/* Generated by tools/generate_dino_audio.py; do not edit. */',
             '#include "dino_audio_assets.h"', '',
             'const uint8_t dino_audio_blob_header[DINO_AUDIO_BLOB_HEADER_BYTES] = {']
    for start in range(0, len(header), 16):
        lines.append("    " + ", ".join("0x%02x" % byte for byte in header[start:start + 16]) + ",")
    lines.extend(['};', '', 'const dino_audio_clip_t dino_audio_clips[DINO_AUDIO_CLIP_COUNT] = {'])
    for clip in clips:
        lines.append("    {%dU, %dU, %dU}, /* %03d %s */" %
                     (clip["offset"], clip["bytes"], clip["samples"], clip["id"], clip["role"]))
    return "\n".join(lines + ['};', '']).encode("utf-8")


def cached_wavs():
    cache = {}
    for directory, filename in ((LEGACY, "manifest.json"),
                                (OUTPUT, "manifest.json"),
                                (OUTPUT, "generation_progress.json")):
        manifest_path = directory / filename
        if not manifest_path.exists():
            continue
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        source = manifest.get("source", {})
        if (source.get("voice"), source.get("rate"), manifest.get("pcm", {}).get("sample_rate")) != (VOICE, RATE, SAMPLE_RATE):
            continue
        for clip in manifest["clips"]:
            path = directory / clip["wav"]
            if path.exists() and digest(path.read_bytes()) == clip["wav_sha256"]:
                cache[clip["text"]] = path
    return cache


def pack_clips(clips, compressed):
    payload = bytearray()
    unique = {}
    for clip, data in zip(clips, compressed):
        # Exact byte equality also protects against a hypothetical hash collision.
        if data not in unique:
            unique[data] = BLOB_HEADER_BYTES + len(payload)
            payload.extend(data)
        clip["offset"] = unique[data]
    header = blob_header(payload)
    return header + payload, header, len(unique)


def build(from_wav=False):
    catalog_bytes = CATALOG.read_bytes()
    clips = narration(json.loads(catalog_bytes))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    compressed = []
    cache = cached_wavs()
    with tempfile.TemporaryDirectory(prefix="dino-audio-") as directory:
        scratch = Path(directory)
        library = compile_decoder(scratch)
        verify_decoder(library, bytes.fromhex("1032547698badcfe"))
        for clip in clips:
            name = "%03d_%s" % (clip["id"], clip["role"])
            wav_path = OUTPUT / (name + ".wav")
            if from_wav:
                pcm = read_wav(wav_path)
            elif clip["text"] in cache:
                pcm = read_wav(cache[clip["text"]])
                write_wav(wav_path, pcm)
            else:
                pcm = synthesize(clip, scratch, wav_path)
            cache[clip["text"]] = wav_path
            data = encode(pcm)
            decoded = verify_decoder(library, data)
            clip.update({"wav": wav_path.name,
                         "samples": len(pcm) // 2, "bytes": len(data),
                         "duration_seconds": round(len(pcm) / 2 / SAMPLE_RATE, 6),
                         "wav_sha256": digest(wav_path.read_bytes()),
                         "pcm_sha256": digest(pcm), "adpcm_sha256": digest(data),
                         "decoded_pcm_sha256": digest(decoded[:len(pcm)]),
                         "pcm_peak": pcm_peak(pcm),
                         "decoded_peak": pcm_peak(decoded[:len(pcm)])})
            compressed.append(data)
            progress = {"source": {"voice": VOICE, "rate": RATE},
                        "pcm": {"sample_rate": SAMPLE_RATE},
                        "clips": clips[:len(compressed)]}
            (OUTPUT / "generation_progress.json").write_text(
                json.dumps(progress, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print("clip %03d: %5.2fs, %6d ADPCM bytes" %
                  (clip["id"], clip["duration_seconds"], clip["bytes"]), flush=True)
    blob, header, unique_count = pack_clips(clips, compressed)
    if len(blob) > MAX_BLOB_BYTES:
        raise ValueError("Audio blob exceeds %d bytes: %d" % (MAX_BLOB_BYTES, len(blob)))
    source = emit_c(clips, header)
    h_source = emit_header(len(blob))
    (OUTPUT / "audio.bin").write_bytes(blob)
    (ROOT / "main/dino_audio_assets.c").write_bytes(source)
    (ROOT / "main/dino_audio_assets.h").write_bytes(h_source)
    manifest = {
        "schema_version": 2,
        "source": {"kind": "macOS offline system speech synthesis",
                   "voice": VOICE, "locale": "zh_CN", "rate": RATE,
                   "quality": "development narration; pronunciation and speaker pending review",
                   "voice_model_included": False,
                   "license_note": "Generated local development audio; no custom narration license claimed"},
        "pcm": {"sample_rate": SAMPLE_RATE, "channels": 1, "bits": 16,
                "encoding": "signed little-endian PCM", "target_peak": PEAK},
        "adpcm": {"encoding": "continuous IMA ADPCM", "bits_per_sample": 4,
                  "nibble_order": "low first", "initial_predictor": 0, "initial_index": 0,
                  "odd_sample_padding": "encode one zero PCM sample; descriptor excludes padding"},
        "blob": {"file": "audio.bin", "bytes": len(blob), "sha256": digest(blob),
                 "header_bytes": BLOB_HEADER_BYTES, "header_hex": header.hex(),
                 "magic_hex": BLOB_MAGIC.hex(), "payload_sha256": digest(blob[BLOB_HEADER_BYTES:]),
                 "offsets": "absolute from beginning of audio.bin", "unique_streams": unique_count},
        "catalog_sha256": digest(catalog_bytes),
        "generated_c_sha256": digest(source), "generated_h_sha256": digest(h_source),
        "total_adpcm_bytes": len(blob) - BLOB_HEADER_BYTES,
        "logical_adpcm_bytes": sum(clip["bytes"] for clip in clips),
        "total_duration_seconds": round(sum(clip["samples"] for clip in clips) / SAMPLE_RATE, 6),
        "clips": clips,
    }
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                           encoding="utf-8")
    return manifest


def verify():
    manifest = json.loads((OUTPUT / "manifest.json").read_text(encoding="utf-8"))
    catalog_bytes = CATALOG.read_bytes()
    if manifest["schema_version"] != 2 or manifest["catalog_sha256"] != digest(catalog_bytes):
        raise ValueError("Catalog/schema changed; regenerate narration")
    if (manifest["source"]["voice"], manifest["source"]["rate"],
            manifest["pcm"]["sample_rate"], manifest["pcm"]["channels"],
            manifest["pcm"]["bits"]) != (VOICE, RATE, SAMPLE_RATE, 1, 16):
        raise ValueError("Narration provenance/PCM format mismatch")
    expected_clips = narration(json.loads(catalog_bytes))
    if len(manifest["clips"]) != CLIP_COUNT:
        raise ValueError("Expected 202 clips")
    blob = (OUTPUT / manifest["blob"]["file"]).read_bytes()
    if (len(blob) != manifest["blob"]["bytes"] or len(blob) > MAX_BLOB_BYTES or
            digest(blob) != manifest["blob"]["sha256"] or
            blob[:BLOB_HEADER_BYTES] != blob_header(blob[BLOB_HEADER_BYTES:]) or
            blob[:BLOB_HEADER_BYTES].hex() != manifest["blob"]["header_hex"] or
            digest(blob[BLOB_HEADER_BYTES:]) != manifest["blob"]["payload_sha256"]):
        raise ValueError("Resource blob/header integrity mismatch")
    compressed = []
    with tempfile.TemporaryDirectory(prefix="dino-audio-verify-") as directory:
        library = compile_decoder(Path(directory))
        for expected, clip in zip(expected_clips, manifest["clips"]):
            for key, value in expected.items():
                if clip.get(key) != value:
                    raise ValueError("Clip metadata/text mismatch: " + str(clip["id"]))
            wav_path = OUTPUT / clip["wav"]
            pcm = read_wav(wav_path)
            offset, size = clip["offset"], clip["bytes"]
            if offset < BLOB_HEADER_BYTES or size <= 0 or offset + size > len(blob):
                raise ValueError("Clip range outside audio blob: " + str(clip["id"]))
            data = blob[offset:offset + size]
            decoded = verify_decoder(library, data)
            checks = [len(pcm) // 2 == clip["samples"], len(data) == clip["bytes"],
                      len(data) == (clip["samples"] + 1) // 2,
                      digest(wav_path.read_bytes()) == clip["wav_sha256"],
                      digest(pcm) == clip["pcm_sha256"], digest(data) == clip["adpcm_sha256"],
                      encode(pcm) == data, digest(decoded[:len(pcm)]) == clip["decoded_pcm_sha256"],
                      0 < pcm_peak(pcm) <= PEAK]
            if not all(checks):
                raise ValueError("Clip integrity/codec check failed: " + str(clip["id"]))
            compressed.append(data)
    repacked_clips = [dict(clip) for clip in manifest["clips"]]
    rebuilt, header, unique_count = pack_clips(repacked_clips, compressed)
    if rebuilt != blob or unique_count != manifest["blob"]["unique_streams"]:
        raise ValueError("Blob packing/deduplication mismatch")
    if any(a["offset"] != b["offset"] for a, b in zip(repacked_clips, manifest["clips"])):
        raise ValueError("Noncanonical clip offsets/deduplication mismatch")
    for path, expected, hash_key in (
            ("main/dino_audio_assets.c", emit_c(manifest["clips"], header), "generated_c_sha256"),
            ("main/dino_audio_assets.h", emit_header(len(blob)), "generated_h_sha256")):
        source = (ROOT / path).read_bytes()
        if source != expected or digest(source) != manifest[hash_key]:
            raise ValueError("Generated asset descriptors do not match: " + path)
    if (len(blob) - BLOB_HEADER_BYTES != manifest["total_adpcm_bytes"] or
            sum(map(len, compressed)) != manifest["logical_adpcm_bytes"] or
            round(sum(clip["samples"] for clip in manifest["clips"]) / SAMPLE_RATE, 6) != manifest["total_duration_seconds"]):
        raise ValueError("Audio totals mismatch")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", help="verify retained WAV/blob/descriptors without synthesis or modification")
    mode.add_argument("--from-wav", action="store_true", help="rebuild resource blob/descriptors from retained WAV without synthesis")
    arguments = parser.parse_args()
    verify_reference_vectors()
    manifest = verify() if arguments.verify else build(arguments.from_wav)
    print("Verified %d clips; %.2f seconds; %d compressed bytes. No speaker playback." %
          (len(manifest["clips"]), manifest["total_duration_seconds"], manifest["total_adpcm_bytes"]))


if __name__ == "__main__":
    main()
