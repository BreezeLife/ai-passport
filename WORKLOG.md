English · [简体中文](WORKLOG.zh_CN.md)

# Work history

## 2026-10-02 — Resume the dinosaur application

Read the shared conversation and all four parent project files. The former prototype had host/browser checks but no integrated ESP-IDF firmware; no source was present here. Retrieved official Gitee main at `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a` into this independent directory and created `feature/dino-passport`. Verified the existing ESP-IDF installation reports v5.5.3. The WeChat article remains inaccessible behind verification. Started implementing the recorded offline experience. No USB port has been opened and nothing has been flashed.

## 2026-10-02 — Integrated firmware and evidence

Completed eight dinosaur cards, sixteen facts, eight retryable questions, footprint collection, CRC-protected NVS, independent Chinese LVGL pages, eight original illustrations and forty-two offline development narration clips. Installed and verified all required Passport skills; retained font/asset sources and provenance. Kept BSP and legacy identity/Recovery reservations. Independent reviews found and verified fixes for redraw retry, muted codec startup, NVS read failures and optional queue degradation.

Build: PASS — fresh complete repository gate under ESP-IDF 5.5.3; application 2,412,096 bytes and merged image 2,477,632 bytes. Exact merged SHA-256: `ece8d04aa6425a3a48a27fd1cfee5e6000c59dcf130bcf51f5755be13d03e8de`; matching BIN/ELF/MAP archive verified.

Host tests: PASS — upstream gate, model 17/17, decoder, 20 actual-main runtime fault scenarios, all required glyphs and all 42 clips; real LVGL 903 state cases and 8,000 repeated renders. Sources, reports and project continuity records retained.

Device tests: NOT RUN. No Passport USB serial port was detected; no port was opened or flashed. Physical display, speech, controls, persistence, heap/stack, power and Recovery, as well as exact article parity, remain open. See [validation](docs/dinobook-validation.md). No commits, pushes or publication.

## 2026-10-02 — Reference body supplied

The user supplied the Ultraman article's body. Reviewed its continuous-motion pipeline, two-sample iteration, exact pause/replay/switch behavior, absence of sound and parent-child observation process. Updated source-availability notes and marked reference review complete. Recorded the current firmware's static-picture and button differences, and a two-dinosaur viewing-page proposal with theoretical Flash/RAM budgets in [the reference](docs/dinobook-reference.md).

This increment changes documentation only. The exact validated firmware remains unchanged; motion samples are not implemented. The existing ZIP preserves the original build-time report snapshot. No device testing or flashing was authorized or performed.

Documentation validation: PASS for local links and language pairing across all 16 touched application documents; `git diff --check` passes. Full repository document check is currently blocked by an unrelated generated `STATUS.md` at the root (disallowed location and missing Chinese peer). That file was preserved, with no rule suppression. No new firmware build was needed for this documentation increment.

## 2026-10-02 — Complete forty-species increment authorized

The user requested Image2 dinosaurs, at least forty species, and complete delivery. Started all forty eight-pose atlases, forty knowledge cards, 202 offline narration clips, V2 legacy-compatible progress, a separate motion page and a dedicated audio resource partition. Plan and assets retain cross-machine continuity. Optional resources and viewing controls are tested against actual source. Kept the prior archive and sibling work. Full forty-species build and physical-device acceptance are still pending; no device write, commit or publication has occurred.

## 2026-10-02 — Forty-species firmware validated

Completed all forty native illustrated atlases and 320 device frames, eighty facts, forty questions, 202 offline speech clips, five footprint pages, V2 migration and motion controls. Corrected the Corythosaurus crest and retained its prior image and edit prompt. Full upstream/application gate passed with recorded Pillow 11.3.0 and ESP-IDF 5.5.3; static CI now prepares that pinned image dependency. Generated Project Pulse output remains preserved and ignored locally; tracked documents remain checked.

Build: PASS — application 2,888,960 bytes, merged 6,715,166; merged SHA-256 `4e8bc60b849cc95b3b89b3a00d351f26674d1257ea8a2844e606e51b33fe8ded`. Matching schema-2 BIN/ELF/MAP/audio archive independently verified. Host: PASS — model31, decoder3, converter5, actual-main46, all591 glyphs and202 clips; real LVGL checked19,608 renders (including8,000 continuous frames), plus9,000 page switches, with zero layout/glyph errors and stable UI memory. Details in [new evidence](docs/dinobook40-validation.md).

Delivery preparation includes full firmware, component/debug archive, complete source, checksums and forty-species offline previews through `tools/create_dino_delivery.py`. Device tests: NOT RUN. No serial port was opened or flashed; no commit, push or publication. The merged FF padding crosses identity/NVS and can erase them; separate compatible segmented writes preserve those regions. Physical colors, speech, buttons, frame timing, total memory, power and Recovery remain open.

## 2026-10-02 — Prepare the authorized GitHub synchronization

Located the existing forty-species working tree on `feature/dino-passport` at official baseline `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`; all prior modifications remain preserved. The older eight-species ZIP is unavailable and was not imported. Reused keyring credentials for BreezeLife and verified push access to its existing public fork. The official GitHub repository is read-only for this account. All 1,391 source hashes were unchanged by verification.

Before publication adaptation, a fresh complete gate passed with ESP-IDF 5.5.3, Python 3.9 and Pillow 11.3.0. The private narrated merged image SHA-256 is `4aa6bb6b6ebcbb534071f77473236406d8bcd0bf97c0759fbfa31d9070d9bf4f`. Independent real-LVGL checks passed 19,608 UI states, 8,000 continuous frames, 9,000 stress renders and 1,675,740 glyph checks, with zero layout/glyph failures and stable UI memory. No device tests or writes.

Review identified Apple system-voice publication restrictions. Keep all local recordings and old archives, exclude the recordings from public Git, and add a buildable public source profile with explicit absent-audio reporting. Align the two catalog READMEs with the current forty-species data. Public-profile validation and Git synchronization are pending.

## 2026-10-02 — GitHub source pushed and verified

Committed the existing forty-species implementation and publication adaptation as [93e9b166536fa64e3ca2aeec509259370ff89d2b](https://github.com/BreezeLife/ai-passport/commit/93e9b166536fa64e3ca2aeec509259370ff89d2b). The non-force push to `BreezeLife/ai-passport` created `feature/dino-passport`; the remote SHA was independently checked. Existing SSH authentication resolved two HTTPS connection timeouts. Gitee origin remains intact and the branch tracks the GitHub fork. No re-login or global authentication configuration changes.

Public source Build: PASS — complete ESP-IDF 5.5.3 gate, merged image and matching archive verified. Host tests: PASS — final public static gate plus real-LVGL checks. Audio asset checks: NOT RUN for public source, explicitly omitted under Apple license restrictions. Complete local narrated assets had passed separately; all 287 original binaries remain unchanged. Device tests: NOT RUN; no port opened or flashing. No firmware/ELF/MAP, recording or historical delivery ZIP was uploaded.

See [GitHub evidence](docs/dinobook-github.md). Remaining physical display/timing, narration, controls, save durability, total memory, power and Recovery checks are recorded there. Public narrated artifacts need redistributable audio.
