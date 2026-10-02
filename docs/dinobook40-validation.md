English · [简体中文](dinobook40-validation.zh_CN.md)

# Forty-species validation — 2026-10-02

Build: PASS. Host tests: PASS. Device tests: NOT RUN.

The complete gate ran from the current forty-species sources under ESP-IDF 5.5.3 in a fresh isolated build directory. Asset checks used the recorded Pillow 11.3.0 through DINO_ASSET_PYTHON; the dependency is pinned for CI. This record supersedes the historical eight-species [build](dinobook-validation.md) for current delivery.

## Exact artifact

| Artifact | Bytes / SHA-256 |
| --- | --- |
| Application | 2,888,960 / 3,145,728; 256,768 bytes free in factory |
| Merged offset-0 image | 6,715,166; `4e8bc60b849cc95b3b89b3a00d351f26674d1257ea8a2844e606e51b33fe8ded` |
| Matching ELF | `fa9930ff7e84792421f46595bdd0a1d279cbdecc4376ea9484ffe43b92c458da` |
| Audio resource at 0x35a000 | 3,200,798 / 3,825,664; `aeb47477daf86f5d9705ab0369d51d33b0de377147ebd624ee88cd381ef58dda` |
| Animation payload | 2,028,800 plus 320 descriptor bytes |

Verified schema-2 archive: `build/firmware/4e8bc60b849cc95b3b89b3a00d351f26674d1257ea8a2844e606e51b33fe8ded/`. Its manifest binds the matching BIN/ELF/MAP, bootloader, partition table, flash arguments and external narration image. Independent read-only archive verification also passed. The old eight-species archive remains unchanged.

## Checks and observations

- Complete upstream repository, workflow, BSP and firmware checks passed. Build/merge registered the real external audio image at its required address.
- Model 31/31, animation decoder 3/3, converter 5/5 and 46 actual-main runtime scenarios passed. Model/decoder sanitizer checks passed. Coverage includes high footprint bits, strict legacy-save migration, input-before-frame accounting, frame deadlines, pause/switch/replay/exit, dim/wake and optional resource failures.
- Catalog: 40 actual dinosaurs, 80 facts, 40 retryable questions and 20/20 balanced answer positions. Museum sources remain in the data.
- Image generation: 40 original atlases using the requested Image2 workflow, 320 selected poses, prompts and unchanged native artifacts. All atlases were visually inspected for whole-body framing and limb changes; Corythosaurus's crest was corrected and the prior version retained. The host does not expose its backend model ID. These are stylized walking illustrations.
- All 320 I4 frames, shared RGB565 palettes, PNG/GIF previews and original hashes passed reproduction and comparison against the compiled C decoder. Frame uniqueness alone does not establish scientific gait accuracy.
- Audio: 202 clips, 800.17 seconds, 3,200,734 compressed payload bytes. Complete WAV/hash/re-encode/actual-C-decode checks passed. Runtime checks the 64-byte resource identity header and bounds; it does not hash the whole payload while playing.
- Fonts: 591 required glyphs at each of 14/18/24 px; known-missing U+9F98 negative control rejected.
- Actual LVGL 9.5.0: 19,608 checked render cases (including 8,000 continuous frames), plus 9,000 page-switch stress renders. No glyph/layout/scale-boundary failures or warnings. Forty species had eight different screen images, 280 adjacent-frame changes, stable pause/replay behavior and reused widgets.
- Continuous-animation pool free bytes stayed 33,088 and largest block 29,600. Peak pool use was 17,760 bytes; post-switch baseline free bytes 33,520→33,528. These are host UI-pool observations, not whole-device heap or stack measurements.

Detailed gate log: `build/dinobook40-full-gate.log`. The actual-LVGL report, 40 GIFs, overview and hash manifest are under `build/preview/`.

## Delivery and physical acceptance

Delivery tool: `tools/create_dino_delivery.py`. The package at `build/delivery/DinoPassport40-2026-10-02.zip` contains verified firmware/debug resources, a complete source snapshot with per-file hashes, and `preview/index.html` for offline viewing. Package and source-ZIP hashes live in its delivery manifests/checksums; they are not recursively embedded in their own source snapshot.

The merged image extends through 0x66771e (erase sectors through 0x668000). Its FF padding covers NVS and identity at 0x356000..0x35a000: a complete offset-0 flash can erase them. Preserving identity/progress requires compatible segmented writes that avoid those data regions. Recovery contents are excluded. No identity/Recovery provisioning, USB-port opening, flashing, commit, push or publication occurred.

Physical display/colors, actual frame timing and loop feel, buttons, narration pronunciation/volume, save durability, total heap/stack, power and Recovery remain unverified. Follow [device acceptance](dinobook.md) only after separate device-write authorization. Computer previews are not device footage.
