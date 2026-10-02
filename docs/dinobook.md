English · [简体中文](dinobook.zh_CN.md)

# Dino Passport use and device acceptance

The complete 40-dinosaur version includes 80 facts, 40 two-choice
observation questions, 202 Mandarin development narration clips, and a separate
motion viewer. All dinosaurs are available from the beginning. A wrong answer
gives a hint and another try; there are no scores or penalties. This guide
records the implementation and resource contracts. Full build and host checks
passed; exact build evidence belongs in
[the validation record](dinobook40-validation.md).

The [parent-child comparison](dinobook-reference.md) describes the supplied
article and the user's later authorization to complete all 40 species. The
motion is an AI-generated walking illustration, not live-animal footage,
scientifically validated locomotion, or a recording from the device.

## Controls

OK starts from the cover. UP/DOWN browse the 40 cards with wraparound; OK opens
the first fact. UP/DOWN switch between two facts and the quiz entry. OK replays
a fact or starts the question. UP/DOWN choose an answer, and OK submits it. A
wrong answer gives a hint and OK retries. A correct answer collects a footprint;
OK shows the stamp, and the next OK discovers the next dinosaur. Long-OK returns
to the card; from a card it opens camp.

Camp offers footprints, sound, motion viewing, and continue. The footprint
album has five pages of eight dinosaurs; UP/DOWN cycle pages with wraparound,
and OK returns to the selected card. Sound is optional and does not prevent
browsing when unavailable.

| Motion-viewer action | Result |
| --- | --- |
| UP / DOWN | Previous/next dinosaur with wraparound; reset to frame zero and preserve paused/playing state |
| OK | Pause or resume |
| Long-OK | Restart at frame zero and resume playing |
| Long-UP | Return to camp |

The motion viewer is silent. Each dinosaur has eight 144×88 indexed frames;
the model schedules one frame every 125 ms, targeting eight frames per second.
This is a configured interval, not a measured device frame rate. Host previews
and GIFs are computer-generated views; GIF centisecond timing alternates
120/130 ms. See [motion sources and conversion](../assets/animations/dinosaurs40/README.md).

## Persistence and resources

The dedicated `dinobook` NVS namespace stores discovered footprints, sound
preference, and the selected dinosaur. V2 is a 24-byte CRC32 record containing
a 64-bit footprint field; only its low 40 bits are valid. Exact 16-byte V1
records are accepted only after checking their magic, version, reserved bytes,
sound value, old eight-dinosaur selection, and CRC. The first eight species keep
their previous order, so their footprints retain their meaning. A later normal
save writes V2; loading does not immediately rewrite the record. Pause, frame,
page, and other transient viewer state are not persisted.

Writes coalesce for 700 ms, so immediate power loss can lose the latest change.
Corrupt or unsupported records fall back to defaults. Unexpected read failures
disable writes for that boot rather than erase shared NVS. Audio and storage
failures remain visible while navigation stays available.

Workers handle codec I/O, battery, and storage; button callbacks only queue
events. Narration is 8 kHz mono IMA ADPCM at speech rate 155. Display facts have
longer text than their short spoken summaries. The ordinary Tingting system
voice is development audio, with volume 35 percent pending speaker acceptance.
Playback completion or cancellation suspends the codec. Backlight dims after
45 seconds idle and turns off after 90 seconds; the first press then only wakes
the screen. This is not a measured deep-sleep mode.

The application keeps the 3 MB factory slot, identity reservation at `0x356000`,
Recovery at `0x700000`, and the five-second UP boot hook. The separate
`dino_audio` data partition begins at `0x35a000`. Its current
`assets/audio/dinobook40/audio.bin` is 3,200,798 bytes including the
64-byte identity header. Firmware compares that header and the expected size
before playback; a mismatch disables narration. This binds resources to the
application build and does not verify the entire payload at runtime. The
offline [audio verifier](../assets/audio/README.md) checks the whole payload.
The earlier eight-image and 42-clip assets remain as history and are not linked
into this 40-dinosaur application.

## Firmware delivery and device acceptance

Run `./tools/validate.sh` under ESP-IDF 5.5.3 and verify the exact archive with
`python3 tools/archive_firmware.py verify <archive-directory>`. When narration is included, the archive must
include the matching external audio resource. The independent actual-LVGL host
renderer checks glyphs, layout, and repeated navigation; its screenshots are
computer renders, not device photographs. Build and host results must be
reported separately from physical-device results.

The merged `0x0` image contains no identity or Recovery payload, but its padded
`0xFF` gaps can erase existing NVS and device identity while writing the entire
range. To preserve existing identity and progress, use compatible segmented
flashing that writes only bootloader, partition table, application, and audio
at their defined offsets. Both routes require separate hardware authorization.
A blank device needs separately provisioned compatible identity and Recovery.
No routine full-chip erase or original-firmware readback is required.

After separate approval, check startup without panic or resets; all 40 species
and Chinese pages; anatomy, full-body framing, palette colors, loop seams, and
actual frame timing; motion wrap, pause, resume, replay, and long-UP exit;
learning controls and all five footprint pages; pronunciation, clarity, volume,
clipping, and audio cancellation; progress after restart and V1 migration;
optional resource/peripheral failures; dim/wake behavior; free heap, largest
block, stack use, and long-run stability; and Recovery entry on a compatible
provisioned device. These physical checks remain unverified until performed.

## Public source profile

The 202 narration clips and artifact sizes above describe the local narrated version. Public source preserves forty-species learning and motion, excludes Apple system-voice recordings, and supports builds without an audio image. A no-bank archive needs no audio image; partition and identity/Recovery protections remain enforced. See [GitHub synchronization](dinobook-github.md).
