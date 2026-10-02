<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

Dino Passport's Source Han Sans subset, OFL license and reproducible conversion are documented in [`fonts/README.md`](fonts/README.md).

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

The 40-species motion sources, Image2 requests, eight-frame atlases, indexed conversion, and host-preview boundaries are documented in [`animations/dinosaurs40/README.md`](animations/dinosaurs40/README.md). The earlier eight static illustrations and RGB565 conversion remain as historical sources in [`images/dinosaurs/README.md`](images/dinosaurs/README.md); they are not linked into the 40-species application.

The editable [dinosaur catalog](data/dinosaurs.json) retains 40 records, museum/research references, anatomy descriptions, 80 display facts, separate short spoken facts, and 40 observation questions. Run `python3 tools/generate_dino_catalog.py --verify` to check the fixed order, text limits, answer alternation, and generated firmware catalog.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

Dino Passport's 202 offline narration clips use `audio/dinobook40/`. The external `audio.bin` is 3,200,798 bytes, including its 64-byte identity header, and is placed at `0x35a000`. The C assets contain descriptors only. Development-voice provenance, 8 kHz ADPCM format, full offline integrity verification, and pending speaker acceptance are documented in [`audio/README.md`](audio/README.md). The older `audio/dinobook/` 42-clip set remains history and is not linked into the current application.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
