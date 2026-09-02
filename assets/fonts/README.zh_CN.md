<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# WorkBuddy 字库

`main/fonts/lv_font_noto_sans_sc_14.c` 是供 WorkBuddy 动态简体中文文本使用的
LVGL 9 压缩字库，字号 14 px、灰度 2 bpp。仓库特意不提交源 TTF。

## 来源与许可

- 字体：Noto Sans SC，取自 Google Fonts 官方仓库的可变字重源文件。
- 固定来源提交：`f6b2b7e8545e086ad3f821af21895d732b6485cf`。
- 来源 URL：
  `https://raw.githubusercontent.com/google/fonts/f6b2b7e8545e086ad3f821af21895d732b6485cf/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf`
- 来源 SHA-256：
  `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`。
- 源文件大小：17,772,300 字节。
- 许可：SIL Open Font License 1.1；再分发所需的完整副本见
  [`NotoSansSC-OFL.txt`](NotoSansSC-OFL.txt)。

生成的 C 子集继续按同一许可再分发。其公开 C 标识符为项目专用名称，不使用保留字体
名称 `Source`。

## 可复现生成

在普通构建之外下载固定版本的 TTF，并提供 `lv_font_conv` 1.5.3。核对源文件
SHA-256 后运行：

```sh
python3 tools/generate_workbuddy_font.py \
  '/absolute/path/NotoSansSC[wght].ttf' \
  --lv-font-conv /absolute/path/to/lv_font_conv
```

若转换器已通过 `LV_FONT_CONV` 指定、安装在
`node_modules/.bin/lv_font_conv` 或位于 `PATH`，可省略 `--lv-font-conv`。
脚本不会访问网络，并会拒绝未经固定的源字体或转换器版本。

字符范围包含可打印 ASCII、GB2312 所有合法双字节序列可解码出的 7,445 个不同
Unicode 字符、常用中文标点、固定 WorkBuddy UI 文案、当前 `main/` 下 C 字符串字面量
中的其他非 ASCII 字符，以及通过 `--extra-text` 显式追加的文本。

生成文件要求 `CONFIG_LV_USE_FONT_COMPRESSED=y`。引用公开符号
`lv_font_noto_sans_sc_14` 时包含
`main/fonts/lv_font_noto_sans_sc_14.h`。生成的 C 文件需由所属 ESP-IDF 组件另行注册；
普通固件构建不会运行生成脚本，也不会联网下载字体工具。

## 生成结果

- 字符数量：7,544 个 Unicode 码点。
- 生成 C 文件大小：2,797,735 字节。
- 生成 C 文件 SHA-256：
  `76b10654b1da6b7277e5e40e5391b1265faa7d96cd2f8e46c7f64f96531a6f64`。
- 使用 LVGL 9.5.0 头文件进行主机编译时，Mach-O 对象大小为 361,024 字节；ESP-IDF
  目标对象和最终固件大小仍取决于交叉编译工具链与链接结果。
