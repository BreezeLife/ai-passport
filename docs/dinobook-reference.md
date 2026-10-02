English · [简体中文](dinobook-reference.zh_CN.md)

# Parent-child reference and the 40-dinosaur increment

## Source and purpose

On 2026-10-02 the user supplied the reference article about a second-grade child
and parent making an animated Ultraman guide. The
[original article](https://mp.weixin.qq.com/s/nAgYftdVR1To3E2gBON89g) had previously
stopped at web verification. The supplied body establishes its workflow and
button behavior. Linked photographs/GIFs and the community release claim have
not been independently checked.

The author's emphasis is the child's participation: describe a desired result,
compare versions, explain a visible problem, revise the task, and check the
result. The adult handles complex tools and device operations. Preserve those
visible decisions and feedback; leave the child time to choose, compare, and
explain what changed.

## Reference workflow

1. Begin with the child's preferred theme and a small usable cartoon prototype.
2. Compare results. The photo-plus-effect stage exposed a mismatch between pose
   and moving light; a concrete observation guided the next revision.
3. Use continuous source motion and check both subject and action. The example
   selected 4.2 seconds at 10 frames per second, yielding 42 frames before
   resizing and compression for offline playback.
4. Make two representative samples, inspect motion, then expand after agreeing
   on direction. Its eventual 37 characters describe that article's scale.
5. Check computer previews separately from device startup, display, and buttons.
   The author reports a white-screen flashing failure repaired in version 1.3.1;
   this does not establish device acceptance for Dino Passport.

## Scope decision and historical candidate

The first Dino Passport build had eight static dinosaur pictures and 42 voice
clips. A later documentation proposal suggested two motion samples, T. rex and
Triceratops, with a separate silent page from camp. Its preliminary candidate
used 42 frames at 144×88, a 16-color palette, and a reusable 25,344-byte RGB565
buffer. The two clips were estimated at 532,288 bytes; this was budget arithmetic,
not an implemented feature or measured runtime result.

The user then explicitly authorized completing all 40 dinosaurs, requesting
Image2 artwork and actions while keeping facts, narration, and quizzes. That
authorization supersedes the two-sample candidate. There is no remaining decision
to approve only two samples. It does not authorize device flashing, commits,
pushing, or publishing.

The current increment therefore targets 40 species, 80 facts, 40 observation
questions, 202 narration clips, five footprint pages, and eight motion frames
per species. Its source requests name `gpt-image-2`; the host tool does not expose
its observed model ID. See the
[recorded generation plan](../assets/animations/dinosaurs40/generation-plan.json)
and [source/format notes](../assets/animations/dinosaurs40/README.md).
The earlier eight-picture and 42-clip resources remain historical material.

## Controls and deliberate differences

| Behavior | Article's guide | 40-dinosaur motion viewer |
| --- | --- | --- |
| Display | Name, continuous action, era/technique | Name and generated walking illustration; learning cards retain era/diet/traits |
| UP / DOWN | Previous/next character, wrap | Previous/next of 40 species, wrap |
| OK | Pause/resume | Pause/resume |
| Long-OK | Restart and resume | Frame zero and resume |
| Switching | Frame zero, retain pause/play | Frame zero, retain pause/play |
| Exit | Not established by supplied excerpt | Long-UP returns to camp |
| Sound | None | Silent viewer; optional narration in learning pages |
| Other activities | Motion viewing | Existing facts, quiz, hint, and footprint loop retained |

A separate camp entry avoids changing the meaning of OK on learning cards.
The viewer's eight frames and 125 ms model interval target eight frames per
second. Neither the article's 42-frame example nor its 10 fps is a mandatory
copying requirement. Frame timing on the actual Passport remains to be measured.

## Illustration and acceptance boundaries

These prehistoric species have no live-animal footage. The generated atlases
are walking illustrations, not a scientifically verified gait reconstruction.
Original atlases and prompts are retained; conversion crops the first eight
cells, preserves full cells, resizes, and quantizes a shared palette. It does not
invent intermediate movement. Distinct frame bytes establish different images,
not believable gait, accurate anatomy, smooth seams, or device timing.

Computer PNG/GIF previews and real-LVGL host screenshots help compare versions.
They must not be presented as device photographs, recordings, or measured device
frame rate. Invite the child to check recognizable features, whether feet move
coherently, whether the body stays complete, and whether a loop visibly jumps.
A disagreement becomes a specific next observation rather than a score.

The 40-species implementation and asset integration are complete. Full gates and
a matching firmware/resource archive have passed; physical-device acceptance
remains separate. Frames, controls, fonts, repeated navigation,
save migration, and resource identity have been checked offline; after separate authorization,
check startup, colors, actual frame timing, buttons, speech, memory, and stored
data on the board. See [the use guide](dinobook.md) and
[exact validation evidence](dinobook40-validation.md); this reference document
never substitutes for a current verified artifact or flashing approval.
