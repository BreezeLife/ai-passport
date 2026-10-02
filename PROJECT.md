English · [简体中文](PROJECT.zh_CN.md)

# Dino Passport

An offline dinosaur field guide for children on FoloToy AI Passport. Browse forty dinosaurs, hear two short facts, answer a two-choice observation question, and collect a footprint. Every dinosaur is open from the beginning; wrong answers give a hint and allow another try without penalties.

The device has a 240 by 320 screen, three ADC buttons and no PSRAM. The application uses its own cream and forest-green pages, licensed Chinese bitmap fonts, compact illustrations and Mandarin narration. UP/DOWN browse or select, OK hears or confirms, and OK long press returns; long press on a dinosaur card opens the camp.

Pure navigation and checksummed persistence live in `main/dino_model.*`; LVGL pages, asynchronous audio and NVS workers live in `main/`. The BSP is reused. Wi-Fi, BLE and microphone recording are outside this offline experience.

Baseline: official Gitee commit `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`, on `feature/dino-passport`. This application deliberately retains the parent project's 3 MB application, identity partition at `0x356000`, Recovery region at `0x700000`, and five-second UP boot hook. The merged image contains no identity or Recovery image; blank devices need their own Recovery provisioning.

Acceptance: complete repository gate, application host tests, Chinese glyph coverage, matching merged/ELF/MAP archive, then separately approved device testing. Compilation does not validate physical display, narration quality, buttons, NVS durability, heap stability or Recovery.

The complete forty-species firmware passed build and host validation on 2026-10-02. See [the validation record](docs/dinobook40-validation.md) for exact artifact identity and [device acceptance](docs/dinobook.md) for controls and remaining physical checks.

The user authorized the complete forty-species Image2 increment under [the plan](docs/dinobook40-plan.md). Forty original atlases supply 320 walking poses; a separate viewing page preserves learning controls. The 202 narration clips use a dedicated data partition. The merged image has FF padding across identity/NVS and can erase their data; preserve them with separately authorized compatible segmented writes. Preserve the child's role in choosing, comparing and explaining results; computer and physical-device acceptance remain separate.

Public GitHub source preserves the forty-species application but excludes Apple system-voice recordings. A clean checkout supports text and motion without narration; local recordings and historical delivery packages stay on this machine. Public narration needs separately licensed assets. See [GitHub synchronization](docs/dinobook-github.md).
