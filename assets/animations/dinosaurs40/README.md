English · [简体中文](README.zh_CN.md)

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
The current resource record is [metadata.json](metadata.json).

Eight unique converted frames are required for every species. Pixel differences
prove that frame bytes differ; they do not establish anatomical accuracy,
believable gait, smooth loop seams, screen quality or real-device timing.
All 40 original atlases were visually reviewed for whole-body framing, limb-pose
changes and identifying traits; [the generation record](generation-results.json)
retains the Corythosaurus crest repair and previous version. This review does not
validate scientific gait. Physical-device acceptance remains open; host previews
are not device photographs.
