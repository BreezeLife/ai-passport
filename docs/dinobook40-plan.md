English · [简体中文](dinobook40-plan.zh_CN.md)

# Forty-dinosaur implementation plan

Goal: complete 40 offline dinosaur entries with generated motion, facts, questions, speech and footprints; deliver verified firmware. The user approved all 40, superseding the two-sample proposal. Flashing, committing and publishing remain separate actions.

Architecture: keep the existing application/BSP split and knowledge loop. Add a camp motion page using eight Image 2 workflow sprite poses per dinosaur, a reusable RGB565 buffer and pure frame timing. Store compact animation in the application and narration in a separate resource partition. Preserve the identity and Recovery reservations and read old eight-entry saves.

## Work and file ownership

- [x] Content: `assets/data/dinosaurs.json`, catalog generator/C, narration generator/C/descriptors and `assets/audio/dinobook40/`. Verify 40 museum-backed entries, two facts each, one two-choice question/hint each, and 202 narration descriptors. Keep the first eight IDs in order. Spoken summaries may be shorter than displayed facts; reference both in the asset manifest.
- [x] Images: one original nine-cell sprite sheet per species under `assets/animations/dinosaurs40/`, first eight cells as sequential poses and ninth as repeated first pose. Preserve 40 prompts, originals, source/model provenance, previews and device encodings. Inspect identity, anatomy, clipping and changed limb poses; repair failed outputs.
- [x] Model: `main/dino_model.*` and model tests. Forty-entry wrap, 64-bit low-40 footprint mask, five footprint pages, motion pause/replay/exit and elapsed-time scheduling. V2 24-byte saves accept V1 16-byte records without erasing NVS.
- [x] Playback: new pure `main/dino_animation.*`, generated animation C and tests/converter. Use 144×88 frames, eight frames, shared 16-color RGB565 palette per species, I4 unpacking to a reusable 25,344-byte buffer; target 125 ms per frame. Never allocate/rebuild the full page every tick.
- [x] Runtime/UI: `main/main.c`, `main/dino_ui.*`, runtime tests and real-LVGL host renderer. Tick the old state before input; long-UP exits motion; entering motion cancels speech. Resource reads stay in the audio worker. Match a 64-byte expected resource header before playback; failed/missing resources degrade speech while browsing remains available. Regenerate/check all Chinese fonts after text changes.
- [x] Build: partitions, root/main CMake, firmware verification/archive tools and tests. Keep the 3 MB factory slot and identity/Recovery reservations; add audio at `0x35a000` with size `0x3a6000`. Register the resource image in flash arguments, include it in merged firmware and retain it in the matching debug archive. Keep old archive-schema verification.
- [x] Evidence/delivery: full repository gate, 40-entry/320-frame asset checks, real-LVGL all-page/glyph/memory checks, matching BIN/ELF/MAP/resource hashes, firmware/source packages and updated project records. Report device tests as unrun until separately authorized.

## Accepted controls

Existing cards/facts/quiz controls remain. Camp adds “motion” before “continue”. Motion cycles all forty with UP/DOWN, resets frame zero on switch while retaining pause, OK pauses/resumes, long-OK restarts and resumes, and long-UP returns to camp. Footprints show eight species per page with UP/DOWN page wrap. Motion is silent and labeled as an illustrative reconstruction.

## Resource and migration checks

Eight 144×88 I4 frames for forty species require 2,027,520 bytes plus 1,280 palette bytes, before indexing/code. Narration must fit the 3,825,664-byte audio partition; target at most 3.7 MB including its 64-byte identity header. Final measured build sizes decide acceptance; arithmetic is not evidence that RAM or frame timing passes.

The new merged image extends beyond the identity reservation: padding may reset identity and NVS. Actual device testing must use compatible segmented writes when identity/data must be preserved, and requires approval. No identity or Recovery payload is generated.

Generated Project Pulse status files remain local outputs. Ignore only their untracked generated paths; tracked and other unignored documents still undergo normal repository/security checks. Preserve existing generated files.
