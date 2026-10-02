<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 恐龙插画

这八张原始 PNG 插画于 2026-10-02 使用内置 `image_gen.imagegen` 工具为
DinoBook 逐种独立生成。未经修改的工具输出保存在 `originals/`；
`prompts.json` 记录完整提示词、物种顺序和格式转换设置。未使用外部素材库
插画或品牌。

插画采用儿童自然科普图鉴风格、森林绿轮廓、柔和自然色与米白背景，每张
只展示一只完整恐龙。选用图像展示了霸王龙的两指短前肢、三角龙的三角与
颈盾、剑龙的背板与尾刺、腕龙较长的前腿、梁龙较低的长颈与细长尾巴、
甲龙的甲片与尾槌、棘龙的背帆与长吻，以及迅猛龙的羽毛与足部大爪。
造型和颜色属于风格化教学插画，并非经过测量的科学复原图。

## 重现设备格式

在仓库根目录、Python 与 Pillow 可用时执行：

```bash
python3 assets/images/dinosaurs/convert_images.py
```

脚本使用 Lanczos 按比例缩放至 216×130 内，保留完整构图，仅在需要时于
米白画布居中留边，再编码为小端 RGB565。它不会重画、删除或增添图像
内容。每张原始像素数据为 56,160 字节，行跨度 432 字节；八张常驻 Flash
的图像数据合计 449,280 字节。

生成的 `main/dino_images.c` 与 `.h` 为 LVGL 9 提供
`const lv_image_dsc_t dino_images[8]`。顺序为霸王龙、三角龙、剑龙、腕龙、
梁龙、甲龙、棘龙和迅猛龙。每个描述符使用 `LV_IMAGE_HEADER_MAGIC` 与
`LV_COLOR_FORMAT_RGB565`。`*-216x130.png` 是普通预览；`.rgb565` 保存
设备使用的确切字节。`checksums.json` 记录原图尺寸及所有原图、转换图的
SHA-256 哈希。

原图与设备预览已在本地检查物种识别特征和全身构图。实际 LCD 颜色、
细节可读性与绘制表现仍需真机验收。
