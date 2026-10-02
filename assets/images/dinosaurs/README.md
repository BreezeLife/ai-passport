<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Dinosaur illustrations

These eight original PNG illustrations were generated separately for DinoBook
on 2026-10-02 with the built-in `image_gen.imagegen` tool. The unchanged tool
outputs are kept under `originals/`; `prompts.json` preserves every full prompt,
the species order, and the conversion settings. No external stock artwork or
brands are used.

The artwork uses a children's natural-history style, forest-green outlines,
soft natural colors, a warm ivory background, and one complete dinosaur per
image. The selected silhouettes show T. rex's two-fingered short forelimbs,
Triceratops's three horns and frill, Stegosaurus's plates and tail spikes,
Brachiosaurus's longer forelegs, Diplodocus's low long neck and slender tail,
Ankylosaurus's armor and tail club, Spinosaurus's sail and long snout, and
Velociraptor's feathers and enlarged foot claws. Shapes and colors are stylized
teaching illustrations, rather than measured scientific reconstructions.

## Reproduce the device format

From the repository root, with Python and Pillow available:

```bash
python3 assets/images/dinosaurs/convert_images.py
```

The script preserves the entire composition while resizing with Lanczos to fit
216×130, centers it on an ivory canvas only if padding is needed, and encodes
RGB565 in little-endian byte order. It does not repaint, remove, or invent image
content. Every raw image is 56,160 bytes, with a 432-byte row stride; all eight
consume 449,280 bytes of Flash-resident image data.

The generated `main/dino_images.c` and `.h` expose
`const lv_image_dsc_t dino_images[8]` for LVGL 9. Array order is T. rex,
Triceratops, Stegosaurus, Brachiosaurus, Diplodocus, Ankylosaurus, Spinosaurus,
and Velociraptor. Each descriptor uses `LV_IMAGE_HEADER_MAGIC` and
`LV_COLOR_FORMAT_RGB565`. The `*-216x130.png` files provide ordinary previews;
the `.rgb565` files preserve the exact device bytes. `checksums.json` records
original dimensions and SHA-256 hashes of every source and converted image.

The originals and device previews were inspected locally for species cues and
full-body framing. Actual LCD color, detail legibility, and rendering still
require device acceptance.
