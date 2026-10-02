<p align="right">
  <a href="firmware-layout.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Firmware Layout

The upstream minimal base targets an ESP32-C3 with 8 MB Flash. Its default
does not reserve product-specific identity, OTA, or unused data partitions.
This DinoBook derivative uses the fixed resource layout documented below.

## Upstream default layout

The upstream default partition table contains exactly:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x6000` | ESP-IDF and application key-value storage |
| `phy_init` | data/PHY | `0xF000` | `0x1000` | PHY initialization data |
| `factory` | app/factory | `0x10000` | `0x7F0000` | The single application image; all remaining Flash |

The default has no OTA slots. This is a starting point, not a restriction on
user firmware.

## Custom layouts

Users may edit `partitions.csv` to resize, move, add, or remove partitions for
their application. A custom table may use OTA slots, filesystem/resource
partitions, or other application-specific data. Keep the 8 MB device boundary,
avoid overlaps, and make sure the application image is flashed at the start of
an app partition large enough to contain it. When a derivative changes its
layout, update that project's documentation and flashing instructions.

## DinoBook resource layout

The current `partitions.csv` contains:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x6000` | Shared and application settings |
| `phy_init` | data/PHY | `0xF000` | `0x1000` | PHY initialization data |
| `factory` | app/factory | `0x10000` | `0x300000` | Application, fonts and compact animations |
| `cardid` | data/NVS | `0x356000` | `0x4000` | Existing device identity reservation |
| `dino_audio` | data/`0x40` | `0x35A000` | `0x3A6000` | External narration bank |
| `recovery` | app/test | `0x700000` | `0x100000` | Existing Recovery reservation |

The root CMake stages `assets/audio/dinobook40/audio.bin` into
`dino_audio/audio.bin` and registers it with ESP-IDF's `flash` target. Therefore
both segmented flash arguments and `idf.py merge-bin` include the same bank.
The validator requires this bank at its partition start and within its bounds,
while retaining the fixed application, identity and Recovery reservations.
No identity or Recovery payload is registered or archived.

The merged image extends beyond the identity region because narration starts
after it. Its `0x356000..0x35A000` hole is FF-filled, **which does not preserve
identity when flashing the merged image**: write-flash erases those sectors.
Recovery contents are beyond the merged image and excluded. To preserve identity
and NVS, use the exact archived component images and compatible segmented writes
that avoid those regions; do not add full-chip erasure. Any device write requires
separate authorization for the exact artifact and data impact. Building and
archiving perform no device writes.

## Enforced validation

Run:

```bash
./tools/validate.sh --firmware
```

The check builds in an isolated directory, creates the merged image, reads the
configured image offsets from `flash_args`, validates the partition-table MD5,
partition bounds, unique labels, and non-overlap, then ensures the application
offset matches an app partition large enough to contain it. It intentionally
does not require the default partition list. CI runs the same gate.

Every image listed in `flash_args`, including user-defined resources and OTA
data, must exist, be nonempty and match the merged bytes at its configured
offset. Image ranges must stay within 8 MB and must not overlap. Additional
images must fit entirely inside a configured partition; an offset inside that
partition is allowed. Merely declaring a resource partition does not require a
preloaded image, but listing an image in `flash_args` makes it mandatory.

Upload only `build/FoloToy-AI-Passport-full.bin`; the similarly named app-only
`build/FoloToy-AI-Passport.bin` does not contain the bootloader or partition
table.

## Flashing and stored data

> **No backup of the firmware already installed on the device is required
> before downloading (flashing) new firmware.** Do not make reading out the
> original firmware or saving a full-Flash dump a prerequisite for this
> workflow. The new firmware replaces the original firmware; this workflow
> does not retain an automatic rollback copy or promise that the original
> firmware can be restored.

Firmware and user data are different. If existing NVS settings, application
records, or files must be kept, export or otherwise save them before flashing
using a method supported by that application. Not requiring an original-firmware
backup does not guarantee data preservation or authorize a full-chip erase.

The verified merged image is written from `0x0`. Because the merged file pads
the gaps between images, flashing it can reset the NVS and PHY data regions.
Use the merged image for blank-device provisioning or an intentional complete
refresh. During normal development, use segmented `idf.py flash` when existing
NVS state should be preserved; this also requires a compatible partition layout
and flash targets that do not overwrite those data regions. `idf.py erase-flash`
erases all user data. Do not add it as a routine prerequisite: use it only when
a complete erase is explicitly intended and any data that must be kept has
been saved.

## Public source without narration

If the local narration bank is absent, CMake omits its flash entry while retaining the same fixed partitions. The public merged image contains only bootloader, partition table and application; its verified erase range determines whether identity is covered. Do not apply the narrated image's identity-erasure statement to a shorter silent image. NVS still lies inside an offset-0 merged flash. Both profiles enforce identity/Recovery payload exclusion and partition bounds. Replacing an application without a resource does not erase a previously installed bank; a matching bank may still be accepted by the runtime. No device write is part of source synchronization.
