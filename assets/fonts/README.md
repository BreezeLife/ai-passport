<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# WorkBuddy font

`main/fonts/lv_font_noto_sans_sc_14.c` is a 14 px, 2 bpp, compressed LVGL 9
font for dynamic Simplified Chinese WorkBuddy text. The source TTF is deliberately
not committed.

## Provenance and license

- Family: Noto Sans SC, variable weight source from the official Google Fonts
  repository.
- Pinned source commit: `f6b2b7e8545e086ad3f821af21895d732b6485cf`.
- Source URL:
  `https://raw.githubusercontent.com/google/fonts/f6b2b7e8545e086ad3f821af21895d732b6485cf/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf`
- Source SHA-256:
  `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`.
- Source size: 17,772,300 bytes.
- License: SIL Open Font License 1.1; the required redistribution copy is
  [`NotoSansSC-OFL.txt`](NotoSansSC-OFL.txt).

The generated C subset is redistributed under the same license. Its public C
identifier is project-specific and does not use the reserved font name `Source`.

## Reproduction

Install or otherwise provide `lv_font_conv` 1.5.3, download the pinned TTF outside
the normal build, verify its SHA-256, then run:

```sh
python3 tools/generate_workbuddy_font.py \
  '/absolute/path/NotoSansSC[wght].ttf' \
  --lv-font-conv /absolute/path/to/lv_font_conv
```

`--lv-font-conv` may be omitted when the executable is specified by
`LV_FONT_CONV`, installed at `node_modules/.bin/lv_font_conv`, or available on
`PATH`. The script performs no network access and rejects both an unpinned source
font and an unpinned converter version.

Coverage is printable ASCII, all 7,445 distinct Unicode characters decodable from
valid GB2312 double-byte sequences, common Chinese punctuation, fixed WorkBuddy UI
terms, additional non-ASCII C string literals currently under `main/`, and optional
text supplied with `--extra-text`.

The generated source requires `CONFIG_LV_USE_FONT_COMPRESSED=y`. Include
`main/fonts/lv_font_noto_sans_sc_14.h` to use the public
`lv_font_noto_sans_sc_14` symbol. Register the generated C file with the owning
ESP-IDF component separately; ordinary firmware builds never run the generator or
download font tooling.

## Generated result

- Character count: 7,544 Unicode code points.
- Generated C size: 2,797,735 bytes.
- Generated C SHA-256:
  `76b10654b1da6b7277e5e40e5391b1265faa7d96cd2f8e46c7f64f96531a6f64`.
- A host compilation against LVGL 9.5.0 produced a 361,024-byte Mach-O object;
  target object and final firmware sizes depend on the ESP-IDF toolchain and linker.
