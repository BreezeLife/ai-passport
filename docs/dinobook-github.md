English · [简体中文](dinobook-github.zh_CN.md)

# Dino Passport GitHub source

The source synchronization uses the existing forty-species application on `feature/dino-passport`, based on official commit `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a`. The older eight-species preview ZIP is unavailable; no import from it is claimed. Existing BSP, artwork, licensed fonts, progress migration and local recordings are preserved.

Destination: [BreezeLife/ai-passport](https://github.com/BreezeLife/ai-passport/tree/feature/dino-passport). Gitee `origin` remains unchanged; GitHub uses `fork` and the existing authenticated account. No force push, firmware upload, PR or device write is part of this synchronization.

## Audio and build profiles

Public source omits local Apple Tingting recordings and the external audio bank under the [audio provenance policy](../assets/audio/README.md). Text, catalog, descriptors and manifests remain available. A clean checkout builds with no narration image; reading, questions, footprints and animation remain available. An existing matching device bank may still be used because a no-bank application build does not erase resources. A complete local bank retains strict size, hash and decoder checks. Publishing narrated firmware requires separately licensed audio.

## Current synchronization validation

Public source Build: PASS. Host tests: PASS. Audio asset checks for public source: NOT RUN (recordings intentionally excluded). Device tests: NOT RUN.

The existing narrated local source passed the full gate before adaptation; its build and prior forty-species evidence are historical local checks, not proof that the new no-bank checkout builds. Final public-source evidence will be recorded here before push. Physical display, timing, pronunciation, buttons, save durability, total heap/stack, power and Recovery remain unverified.

Validated the public Git inventory in an independent checkout containing zero narration binaries. ESP-IDF 5.5.3 complete gate passed; schema-1 archive verification passed. Application: 2,888,960 bytes. Public merged image: 2,954,496 bytes; SHA-256 `5ead5c079a6e08ae58339373c54a8defaa874b6902cd42f05ae9baf2c546cd9d`. Matching ELF SHA-256: `05907cf2e8c58f8e40e18c075d102463a112dd42499f11900994e4deae0d4f52`. Only the three base images are registered. The image ends at `0x2d1500`, before identity/Recovery; an offset-0 write would still cover NVS. These private verification outputs are not committed or uploaded.

Firmware verifier 54/54 and archive tests 33/33 passed, alongside all upstream/application host checks, 591 glyphs at all three sizes and 40/320 animation assets. The unchanged UI implementation separately passed 19,608 real-LVGL cases, 8,000 continuous frames, 9,000 stress renders and 1,675,740 glyph checks, with zero layout/glyph failures. All 287 original narration binaries retain their pre-task hashes. No device access or flashing.
