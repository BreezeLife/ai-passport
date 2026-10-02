English · [简体中文](README.zh_CN.md)

# Dino Passport development narration

The current [dinobook40](dinobook40/) directory contains 202 offline Mandarin
speech clips for 40 dinosaurs. They use the ordinary macOS system-synthesized
`Tingting` (`zh_CN`) voice at rate 155. This is development narration;
pronunciation and delivery have not been accepted on the device. Generation
saves files only and never sends audio to a speaker. No Apple voice model or
system package is included, and no custom narration redistribution license is
claimed. The earlier [dinobook](dinobook/) 42-clip version remains as source
history and is not the current firmware resource.

The [manifest](dinobook40/manifest.json) records every original spoken text,
voice provenance, PCM format, clip ID, duration, sample count, absolute resource
offset, and SHA-256 hash. Dinosaur facts and observation questions are adapted
from the official museum and research references linked in the
[catalog](../data/dinosaurs.json). Display facts contain 25–40 Chinese characters;
`spoken_facts` are separate 16–20-character summaries of the same observations.
The application authors wrote the welcome and discovery prompts.

| Clip IDs | Content |
| --- | --- |
| 0–39 | Dinosaur names in catalog order |
| 40–119 | Two spoken facts: `40 + dinosaur * 2 + fact` |
| 120–159 | Question followed by both numbered options |
| 160–199 | Observation hint after an incorrect choice |
| 200 | Discovery and footprint announcement |
| 201 | Welcome |

## Source and resource formats

Each retained WAV is 8 kHz, signed 16-bit little-endian mono PCM. This format
keeps the speech resource small without increasing the narration speed. The
generator normalizes the peak to approximately 12,000, preserves a 20 ms margin
around detected speech, and adds 100 ms leading and 180 ms trailing silence.
Temporary AIFF intermediates are not retained. WAV files permit later editing
and encoding without synthesizing again.

`dinobook40/audio.bin` is the external audio partition image. Its
64-byte identity header contains eight magic bytes (`DINOA40` followed by zero),
a little-endian 32-bit total blob size, the 32-byte SHA-256 of the payload, and
20 reserved zero bytes. All descriptor offsets are absolute positions in this
file and start at or after byte 64. Identical encoded streams share an offset.
[The C file](../../main/dino_audio_assets.c) contains only the expected identity
header and 202 descriptors; it does not embed the compressed audio payload.
The current blob is 3,200,798 bytes and represents about 800.17 seconds.
The exact resource size and hash are in the manifest and
[generated header](../../main/dino_audio_assets.h).

Every clip is a continuous 4-bit IMA ADPCM stream with low nibble first, initial
predictor zero, and initial index zero. There are no per-clip WAV or block
headers. The first nibble encodes the first PCM sample. When the sample count is
odd, an additional zero PCM sample is encoded to finish the last byte; the
descriptor's `samples` excludes that padding. Whole-byte decoding chunks retain
the decoder state, and playback stops after the stated real sample count.

At startup, firmware compares the partition's identity header with the expected
header before playback. This identifies resources from a different build; it
does not calculate a complete payload hash on the device. The offline verifier
checks the entire blob hash and every clip. Streaming reads and small PCM
buffers avoid loading whole clips into RAM.

## Regeneration and validation

Use Python 3.9 or later and a host C compiler. Synthesis additionally requires
macOS `say` and `afconvert`; verification and WAV re-encoding use only the Python
standard library and compiler. Python 3.13 is supported without `audioop`.

```bash
python3 tools/generate_dino_catalog.py --verify
python3 tools/generate_dino_audio.py
python3 tools/generate_dino_audio.py --from-wav
python3 tools/generate_dino_audio.py --verify
```

Generation uses `say -v Tingting -r 155`, followed by
`afconvert -f WAVE -d LEI16@8000 -c 1`. It reuses an existing WAV only when the
text, voice, speech rate, sample rate, and saved file hash match. The
`generation_progress.json` checkpoint records completed text and file hashes
for resumable generation. A stalled `say` call has a 30-second timeout and up to
three attempts. `--from-wav`
rebuilds the blob, descriptors, and manifest from the retained WAV files.
`--verify` neither synthesizes nor modifies assets: it checks catalog text,
source hashes, PCM format, exact sample counts, re-encoding, payload/header
integrity, deduplication offsets, generated C/header, and every decoded sample
against the actual C decoder. The independent standard-library IMA reference
was cross-checked against Python 3.9 `audioop` for the earlier 42 WAV files and
fixed signed, odd-length, and decode vectors.

The [decoder host test](../../tests/test_dino_adpcm.c) covers reference vectors,
nibble order, saturation, invalid state and arguments, insufficient capacity,
overflow-sized input, and consistency across decoding chunks. Firmware and
audio resources must be delivered together through the repository packaging
workflow; replacing an application alone may leave an incompatible old resource.

## Pending acceptance

Host checks establish codec and file consistency, not pronunciation or speaker
quality. Review all dinosaur names, uncommon vocabulary, the order of spoken
options, intelligibility, volume, clipping, and playback cancellation on the
actual device. Pay particular attention to uncommon characters and long names
in the 40-name set. Device playback and pronunciation acceptance remain pending.

## Public source and local recordings

The public GitHub source excludes both generations of Tingting WAV/ADPCM recordings, the external `audio.bin`, and the local synthesis checkpoint. The files remain in the developer's working directory; text, provenance, hashes and descriptors remain versioned. [Apple's macOS Sequoia license, section 2F](https://www.apple.com/legal/sla/docs/macOSSequoia.pdf) restricts public sharing of System Voices. These recordings and firmware/delivery ZIPs containing them must remain local personal-development artifacts.

A clean checkout builds without the narration bank. CMake reports the omission and does not register an audio image; runtime rejects an absent or incompatible bank and keeps the text, questions, footprints and motion pages available without narration. The static gate reports `Audio asset checks: NOT RUN` when both the bank and source WAVs are absent. Partial local resources still fail verification. Local complete resources retain full checks; this does not grant redistribution rights.

Use the macOS generator only for local personal use. Public narration requires separately licensed recordings, matching provenance, regenerated descriptors and complete asset verification before including an audio bank. Public narration and device pronunciation are pending. The historical delivery tool requires the complete local narration resource and is not a public silent-profile packaging workflow. See [GitHub validation](../../docs/dinobook-github.md).
