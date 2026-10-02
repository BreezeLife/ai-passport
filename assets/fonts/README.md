English · [简体中文](README.zh_CN.md)

# Dino Passport fonts

Uncompressed 2 bpp LVGL 9 subsets at 14, 18 and 24 pixels cover the fixed 40-dinosaur application text. The current inventory contains 591 distinct characters, including printable ASCII, dinosaur names, facts, questions, hints, and motion/footprint controls. The source is SourceHanSansSC-Normal.otf distributed with pinned LVGL 9.5.0, SHA-256 `1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`. Adobe Source Han Sans uses SIL OFL 1.1; see SourceHanSansSC-OFL.txt. The licensed source is retained for rebuilding.

Install `lv_font_conv@1.5.3` with `npm install --prefix . --no-save --package-lock=false lv_font_conv@1.5.3`, then run `python3 tools/generate_dino_fonts.py`. The `--check` mode validates fixed-text coverage and a known-missing negative probe. [`characters.txt`](characters.txt) records the generation inventory. The generator collects non-ASCII strings in `main/dino_*.c` plus printable ASCII; it does not support arbitrary input.

Every label selects its font explicitly. Actual line heights are 17, 21 and 28 pixels. Fonts are linked through main/CMakeLists.txt and remain in Flash. Physical LCD rendering remains a device check.
