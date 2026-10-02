English · [简体中文](dinobook-validation.zh_CN.md)

# Historical eight-species validation — 2026-10-02

This records the earlier static-picture build. Current forty-species delivery is documented in [the new record](dinobook40-validation.md).

Build: **PASS**. Host tests: **PASS**. Device tests: **NOT RUN**.

The complete `./tools/validate.sh` gate ran on macOS with ESP-IDF 5.5.3 in a fresh isolated build after the final queue-allocation fix. It retains upstream repository, workflow, BSP, demo and firmware-tool checks. All five required Passport skills have verified repository-local links. The actionlint installer was exercised in a fresh destination with its pinned archive checksum; macOS fixes retain hash validation and host linker dead-code elimination.

## Exact firmware

Target: ESP32-C3, 8 MB Flash, no PSRAM. Official Gitee baseline: `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`. Branch: `feature/dino-passport`; uncommitted working tree; embedded version: `0b9e4c8-dirty`.

| Artifact | Bytes | Placement |
| --- | ---: | --- |
| `FoloToy-AI-Passport-full.bin` | 2,477,632 | Flash at `0x0` |
| `FoloToy-AI-Passport.bin` | 2,412,096 | Application at `0x10000` |
| `bootloader/bootloader.bin` | 21,376 | `0x0` |
| `partition_table/partition-table.bin` | 3,072 | `0x8000` |

The application fits the 3,145,728-byte factory slot with 733,632 bytes free.

Merged SHA-256: `ece8d04aa6425a3a48a27fd1cfee5e6000c59dcf130bcf51f5755be13d03e8de`.

Matching ELF SHA-256: `00b412171cd0529f023fbf98210a168482bb7483884cb4c367ff8106cbb5e55e`.

BIN, ELF, MAP, bootloader, partition table, flash arguments and hash manifest are archived in `build/firmware/ece8d04aa6425a3a48a27fd1cfee5e6000c59dcf130bcf51f5755be13d03e8de`. Separate archive verification confirmed hashes, layout and embedded ELF identity. The delivery ZIP includes this archive, both build-time report snapshots, gate output and host preview; its reports predate the supplied article body and the later reference comparison.

The merged image is not a full 8 MB device dump. It ends before identity at `0x356000` and Recovery at `0x700000` and contains neither payload. Padded gaps include NVS; merged flashing can reset settings/progress. The retained legacy `app/test` partition named `recovery` triggers ESP-IDF's name/subtype warning. Recovery installation, capacity and entry remain unverified; see [device acceptance](dinobook.md).

## Application checks

- Model: 17/17 cases, 4,096 persisted-state round trips, single-bit corruption and valid-CRC invalid-field rejection, and 262,144 input transitions. Model and ADPCM decoder also passed ASan/UBSan.
- Actual `main.c`: 20 deterministic fault scenarios under strict C11 warnings-as-errors, including redraw after lock failure; NVS init/read/write/commit failures without erase; 8 kHz audio, cancel/replacement and muted startup; task/button/queue allocation failures; codec init/wake/write failures. Optional queue failure retains browsing and idle screen-off.
- Fonts: each 14/18/24 px font covers the 413 currently required characters; the absent U+9F98 negative probe rejects. Source, OFL license and generation inventory are retained.
- Audio: all 42 clip hashes, text, descriptors, reference encoding and actual C decoding verified; 1,167,929 compressed bytes, 291.979125 seconds. No speaker playback occurred.
- Assets: eight original generated illustrations retain prompts, source/converted hashes and RGB565 data. Museum references remain in the catalog; speech is identified as system development narration.

## Real LVGL host render

The harness compiles the actual UI, images and fonts with LVGL 9.5.0. It passed 903 page/state cases and 8,000 repeated renders: 59,955 labels and 447,065 glyph checks, with no placeholder glyph, truncation, overflow, allocation failure or LVGL warning/error. Forty-eight selected frames under `build/preview/` are computer renders, not device photographs.

Configured pool: 49,152 bytes; allocator-usable total: 45,576; peak used: 17,720; baseline free: 33,600; final free: 33,632. No increasing UI allocation was observed. This does not measure total device heap or physical task stacks.

Reproduce with `python3 tools/run_dino_ui_host.py --lvgl-source /path/to/local/lvgl-9.5.0`; CMake, host compilers and Pillow are required. The script does not download dependencies.

## Open acceptance

At build time the WeChat article was unavailable behind verification. The user subsequently supplied its body; [the comparison](dinobook-reference.md) establishes its workflow and records the animation/control features absent from this exact firmware. Prior application source was unavailable, so this implementation reconstructs the recorded shared prototype. Linked article media and community release status have not been independently checked.

No device port was opened or flashed; no Passport USB serial port was detected. Physical startup, Chinese LCD/color, pronunciation, speaker quality/volume/cancellation latency, button timing, restart/power-loss persistence, battery, whole-device heap/stack, power and Recovery remain unverified. Host stubs do not reproduce RTOS concurrency, I/O timing or physical power loss. Device testing requires separate authorization under the original task instruction.
