[English](README.md) · 简体中文

# 恐龙护照中文字库

未压缩的 2 bpp LVGL 9 子集为 14、18、24 px，覆盖 40 种版本的固定应用文字。当前字符集为 591 个不同字符，包含可打印 ASCII、恐龙名字、知识、问题、提示以及动作和足迹操作。源字体为 LVGL 9.5.0 配套的 SourceHanSansSC-Normal.otf，SHA-256 是 `1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b`。Adobe 思源黑体采用 SIL OFL 1.1，见 SourceHanSansSC-OFL.txt；完整许可源字体保留供重建。

运行 `npm install --prefix . --no-save --package-lock=false lv_font_conv@1.5.3` 安装固定转换器，再运行 `python3 tools/generate_dino_fonts.py`；`--check` 验证固定字符覆盖及已知缺字反例。[`characters.txt`](characters.txt) 保存生成字符集。脚本收集 main/dino_*.c 中非 ASCII 字符和可打印 ASCII，不支持任意输入。

每个标签显式选用字库，真实行高为 17、21、28 px。字库通过 main/CMakeLists.txt 编译，常驻 Flash。LCD 中文显示仍须真机检查。
