# Direct scenario testing

Launch a field or encounter through the current build's normal engine and
asset loaders. No playthrough, emulator CPU snapshot, or old executable is
needed. This supports testing later discs as soon as their loaders work.

Find an encounter in your extracted disc:

```sh
python3 tools/scenario_catalog.py disks/us/disc3/FF9.IMG --disc 3 --find Ark
# disc 3  battle 0  Ark
```

Run a fixture on the host, retaining the game's actual failure exit code:

```sh
tools/docker.sh make -j16 pc
tools/docker.sh build/pc/ff9ds --scenario tests/scenarios/disc3_ark.cfg --frames 12000 --status 1000
tools/docker.sh python3 tools/scenario_smoke.py tests/scenarios/*.cfg
```

The batch runner writes individual logs and `report.json` under
`build/validation/scenarios/`. A missing routine, crash, timeout, or failure to
reach the requested frame limit is a failure. Passing this execution check
does not establish visual correctness or DS performance.

For native DS tests, copy the fixture to the SD filesystem and compile its
path into the test ROM:

```sh
tools/docker.sh make -j16 ds AUTORUN=1 ITCMGUARD=1 SCENARIO=sd:/ff9ds/scenarios/disc3_ark.cfg
```

The selected raw disc must be present at `sd:/ff9ds/raw/FF9 CD3.img`.
melonDS folder sync does not grow an existing auto-sized SD image reliably
when another disc is added. Use a newly created SD image with a folder
containing the selected disc, or an image sized for all desired discs.
Use one emulator at a time. Capture screenshots and short videos on DS and
the matching PSX encounter; emulator saves remain useful for inspecting a
precise failure in the same build.

For an animation at a known game frame, arm the recording before that frame
rather than estimating wall-clock delays. This works after direct scenario entry
or a scripted opening run on Linux with melonDS's fast-memory backend:

```sh
DISPLAY=:99 XAUTHORITY=/dev/null python3 tools/capture_ds_at_frame.py \
  --pid 12345 --window 0x200007 \
  --rom build/validation/test.nds --elf build/validation/test.elf \
  --frame 4800 --state-out build/validation/pre_action.ml8 \
  --seconds 60 --out build/validation/action.mp4
```

Use the actual emulator PID/window/display and its matching archived ROM/ELF.
The helper checks the process's ROM argument, native instructions and RAM layout,
then polls the live game counter. A passed threshold, mismatched build, backwards
counter or timeout aborts before capture. An optional fresh checkpoint is saved
first; its JSON reports the actual saved frame, so leave a lead before the action.
Checkpoints remain tied to their executable. The tool does not pause game logic;
review the resulting footage to verify that it includes the intended action.

Fixtures use `key=value`, with comments beginning with `#`:

| Key | Meaning |
| --- | --- |
| `kind` | Required `field` or `battle` |
| `disc` | Disc 1–4; defaults to 1 |
| `field` | Field ID; defaults to opening field 70 |
| `battle` | Required encounter ID for battles, resolved against that disc |
| `party` | Four character indices, 0–8 or -1 for empty; first slot occupied |
| `level` | Optional level 1–99, rebuilding stats and healing |
| `hp.SLOT` | Override current HP after leveling, bounded by maximum HP |
| `equip.SLOT` | Five equipment item IDs, 0–255 |
| `item.ID` | Set inventory quantity 0–99, preserving other items |
| `story` | Set the 16-bit event story counter |
| `event.OFFSET` | Set an individual event byte, offsets 0–0x7ff |
| `characters` | Exact 9 × 0x90-byte canonical character records |
| `events` | Exact 0x800-byte canonical event variables |
| `seed` | Override initialization RNG seed; defaults to 1 |

Blob paths are interpreted relative to the process working directory; use
absolute SD paths on DS. Imported records contain no runtime pointers. Blob
values are applied first, then explicit overrides. Without blobs, the normal
New Game initializer supplies the baseline. The Ark and Meltigemini fixtures
are synthetic combat setups, not canonical late-game saves. Enemy names list
types available in a bundle; story flags can select formations or scripts.

Current limits: battle fixtures exercise isolated encounters; returning to a
fully restored parent field/world state is not yet implemented. Field story
and spawn requirements still need appropriate fixture flags. RNG seeding
does not by itself guarantee deterministic wall-clock/input behavior.
Canonical save-data extraction and scripted action/phase assertions will
extend these fixtures; loading a boss is not equivalent to covering all of
its attacks, victory, defeat, or cutscene transitions.

Verified: opening field 70 entered directly on the host; disc-3 Ark entered
directly on both host and melonDS, revealing optional null default-motion
handling and missing battle effect emitters without playing the opening.
The two Ark emitters D4A30 and D45FC now have native equivalents that pass
original-MIPS packet comparisons. Ark subsequently reaches an unresolved
CD-loaded MIPS effect module: its header pointer at PSX 801E7700 is 801E9058,
initializer is zero, and first effect callback is 801E8178. The generated
resident overlay relocations do not cover this module. The 12000-frame batch
therefore reports Ark as a crash, and Meltigemini as missing D3ED4; neither
boss is claimed to pass. Reports live in
`build/validation/scenario_emitters_batch/`. The matched native test ROM/ELF
is `ff9ds_scenario_ark_emitters`, with a fresh-boot capture in
`build/validation/ds_direct_disc3_ark_fresh_boot.mp4`.

The subsequent `effect_vm.c` bridge executes integer/GTE glue from these
CD-loaded effect modules and calls their native battle-engine exports. The
reviewed pointer-argument masks preserve scalar flags and optional NULLs;
D044C's second argument is an output pointer carried as `s32`. Resident
callbacks keep their native entry points. The bridge passes 300 differential
integer/branch-delay/GTE-model cases, 60 cases of Ark's original initialization
code, and the native 16-word argument test under ASan/UBSan. It does not yet
claim complete MIPS-I instruction/load-delay coverage or support for every
module/export signature. Ark now reaches missing D5C30 on the host; the
original dynamic-header crash is passed, but its full attack remains unported.
Fresh ARM validation reproduces that later missing D5C30 stop at frame 1595
(`ds_ark_effect_vm_probe.ml8`, matching `ff9ds_ark_effect_vm` ROM/ELF), with
the first-fault marker zero and no instruction-region corruption. The existing
60000-frame opening-to-briefing host route also passes with this bridge.

## Subsequent Ark effect coverage

D5C30's circular-chain ribbon emitter and the shared DFF5C/DFF64 particle
integrator now have native equivalents. The differential suite compares
context bytes, packets, OT links and raw production GTE state against original
MIPS in 400 randomized cases under ASan/UBSan. D3ED4's textured effect quads
also pass the expanded 400-case quad/sprite suite (shared GTE model).

Further Ark failures exposed two portability details: the export interface's
model-pool initialization arguments are pointers into the loaded module; and
E1534 reuses effect descriptors whose pointer fields carry the PSX sign bit
after relocation. Native pointers lack that bit, so repeated initialization
added the base twice. Reviewed export masks now map model-pool arguments, and
counted descriptor patches only relocate values still within the offset range.

Fresh DS execution reaches frame 3051, executes attacks and reduces party HP,
then stops at missing D4228. The matching `ff9ds_ark_ribbons_particles` ROM/ELF
and `ds_ark_ribbons_particles_combat.ml8` retain the failure. Its first-fault
marker is zero and the ITCM instruction region matches the ELF. Videos:
`ds_ark_ribbons_particles_boot.mp4` and
`ds_ark_ribbons_particles_combat.mp4`. This does not prove the entire boss,
exit paths, visual fidelity, or performance. The host batch still reports Ark
and its low-HP variant as missing D4228, and Meltigemini as a later segmentation
fault (`scenarios_after_ribbons/report.json`).

## Matching original PSX encounters

The original PSX also enters Ark directly from a disc-3 field baseline using
the host fixture's pointer-free character/event records and ordinary battle
resources. `tools/psx_battle_fixture.py` reads encounter/background sectors
from the original disc, unwinds the original field before importing synthetic
story data, and stops at the resident dispatcher before the battle loader.
It changes runtime test data, preserving executable bytes.

Start Redux with disc 3 paused, then run:

```sh
python3 tools/psx_battle_fixture.py tests/scenarios/disc3_ark.cfg \
  --characters build/validation/ark_fixture_characters.bin \
  --events build/validation/ark_fixture_events.bin \
  --base-state disc3_scenario_field_ready
```

Export these exact-size blobs after `port_scenario_bootstrap` returns in the
host build: `GameData.characters` (9 x 0x90) and `D_8007AEF0` (0x800). The
referenced local baseline was obtained from the existing disc-3 Pandemonium
memory-card save, without a playthrough. A cold Redux BIOS must boot before
restoring it; otherwise this instance restores RAM/VRAM but remains in a BIOS
loop. The tool boots a cold BIOS before its own baseline restore. Do not start
Redux with `--state` before booting for this path. A pause in a currently live,
initialized field also works without `--base-state`.

The tool is runtime-validated after warm boot (`psx_fixture_warm_boot.log`,
`psx_fixture_warm_boot.png`) and from a cold paused start through automatic
boot (`psx_fixture_auto_boot.log`, `psx_fixture_auto_boot.png`), independently
of its offline range check. The
original entrance/combat video is `psx_direct_ark_entrance_combat.mp4`; the
Photon HUD screenshot is `psx_direct_ark_hud_reference.png`. Party/story data
match; input timing and RNG phases are not synchronized, and inventory is not
imported by this PSX helper.

## Cached HUD text and subsequent effect interfaces

The earlier screenshot-based diagnosis of stale/overlapping DS name text was
incorrect. Replaying the saved PSX VRAM through the scaler reproduces the active
DS bitmap exactly, including the name row (`ds_ark_scaler_replay.json`: zero
different pixels). Cached battle names and HP/MP sprites bypassed the font
coverage matcher and received nearest sampling instead. The matcher now includes
the reviewed atlas bounds for cached names, cached values, and direct digits.
Its positive/negative UV tests pass under ASan/UBSan. A fresh DS state at frame
2483 records 64 coverage rectangles in each frame list, including four names
and eight cached values, with no first-fault marker or ITCM corruption.
`ark_hud_same_frame_comparison.png` compares old/new filtering on identical
source pixels; `ds_ark_hud_coverage_boot.mp4` and
`ds_ark_hud_coverage_combat.mp4` retain live animation. This establishes coverage
of these sprites, not complete visual fidelity or acceptable performance.

D4228/D437C now emit native projected sprites/points and pass 600 original-MIPS
CPU/packet cases using a shared GTE model. Subsequent failures exposed raw PSX
positions and output buffers inside loaded modules, export-table entries beyond
the former 160-entry scan, and six/nine-argument callbacks. The bridge scans
216 function entries (data follows), decodes reviewed pointer arguments while
preserving scalar bit patterns, and supports up to 16 o32 callback arguments.
Local descriptor reads and D1494 buffer accesses decode pointers without changing
the module's stored pointer representation. Tests exercise late exports,
six/nine incoming arguments, zero-padding of native arguments, and 16-word
outgoing calls. Fresh native DS execution with these changes reaches frame 4251
and stops at missing D57D0, with no first-fault marker or changes to the first
16 KiB of ITCM (`ds_ark_trail_writer_stop.ml8` and inspection JSON, matching
`ff9ds_ark_trail_writer` ROM/ELF). Boot and combat videos with the same prefix
retain animation. This validates the prior pointer/callback fixes along this
route, not all module interfaces.

CF5A4 now writes circular trail positions/widths; 400 randomized cases compare
the entire buffer and return value with original MIPS, including wraparound,
byte counter rollover, and raw/native input pointers. The host Ark route now
passes the prior failures and stops at missing D57D0
(`scenarios_trail_writer/report.json`). The encounter and later content remain
incomplete; neither this progress nor a passing build proves the boss playable.

D57D0 now emits the single textured effect rectangle, preserving UV arithmetic,
context scratch writes, the existing MAC0 cull decision, depth bucket limits,
OT links and optional color fade. Its original-MIPS differential cases expand
the quad suite to 700 cases using a shared GTE model. Both targets build. The
host proceeds past frame 6000 before a raw output pointer to E750C causes a
segmentation fault (`pc_ark_d57d0_fault.log`); D57D0's native DS rendering still
requires validation.

Reviewed unit-position exports now decode position/rotation/vector buffers at
table offsets 1F8, 1FC, 200, 204, 20C, 214, 2A0, 2A8 and 2B8. Model draw
exports at 60, 64, 28C and 31C also decode their first three arguments: the
third is declared `s32` in wrappers but forwarded as a scale vector to DCA68.
Summon bone output buffers at 254/290 receive the same treatment. Fifteen
synthetic export cases verify mapping, native writes and unchanged scalar IDs.
The existing 360-case effect VM suite and both target builds pass. Both Ark
fixtures advance to a different failure: BFAF4 dereferences a null `curCmd`
while processing status bit 0x400 (`pc_ark_positions_models_fault.log`).
This interface batch still requires fresh native DS validation.

BFAF4's status-bit-0x400 command-owner read now uses the PSX low-RAM shadow
when `curCmd` is NULL. Original MIPS at BFE94 loads Battle+1088 and BFE9C reads
word +4 without a NULL guard. The matching source is unchanged. Both target
builds and low-RAM tests pass; normal and low-HP Ark fixtures each complete
12000 host frames. At 65000-frame limits both stop earlier in battle-end window
setup at missing 63118; Meltigemini now stops at missing D4768
(`scenarios_status_curcmd_extended/report.json`).

Fresh native execution of D57D0 and the unit/model export fixes also reaches
missing 63118 (`ds_ark_positions_models_current.png` and failure video).
Savestate hotkeys failed twice in that emulator instance, so no saved frame
count, fault marker or ITCM comparison is available for this run. The file
named `ds_ark_positions_models_combat.mp4` was captured after failure and is
not evidence of combat animation. The earlier trail-writer videos remain the
last verified live combat recordings. These encounters, post-battle UI,
graphics and performance still need further work.

The native 63118 entry now sets a window's tween/delay/frame and optionally
rewinds coordinates by signed animation deltas. It is the non-BIOS entry
inside the original 63108 assembly body. Four hundred cases compare the
complete window record against original MIPS, with a shared signed-delta sum,
including NULL animations/delta arrays, negative counts, byte-masked flags,
and coordinate wraparound. Both targets build. Ark's longer host routes now
reach `gameover_main`, which submits ordering tables and packets from two
stack-local `DispBuf` structures; DrawOTagEnv's ordinary arena-only link
resolution rejects those native stack links (`pc_window_setup_ot_fault.log`).

Game Over now submits its two stack-local display buffers through the bounded
native OT walker, after installing the draw environment. Both target builds and
the existing native-stack/arena/alias ordering-table regressions pass. Normal
and low-HP Ark fixtures each complete 65000 host frames, remaining in mode 8's
Game Over wait loop (`scenarios_gameover_stack_ot/report.json`). This verifies
execution through defeat, not victory rewards or dismissal back to the title.

Original PSX defeat was reached through ordinary combat after lowering only
the living party units' HP to 1 (`psx_ark_defeat_hp_override.json`); executable
bytes, statuses and enemy data were preserved. The screenshot is
`psx_ark_gameover_reference.png`,
and original state `disc3_ark_gameover_fixture`. The host's rendered Game Over
frame at 15017 matches every pixel at native RGB15 precision. PNG channels
differ by at most 7 because PSX expands the low channel bits while the host
shifts by three (`gameover_psx_host_comparison.json`). This comparison covers
the static Game Over composition, not all combat rendering.
The file named `psx_ark_defeat_transition.mp4` was captured too late: it shows
static Game Over with the emulator's debug UI, so it is not evidence of the
defeat entrance animation. The subsequent GPU-only dismissal video below is
the original PSX animation reference.

Fresh DS execution with 63118/native Game Over buffers has reached frame 9751,
including KO and subsequent Ark attacks, with no first-fault marker or changes
to the first 16 KiB of ITCM (`ds_ark_window_gameover_posteffects.ml8`, matching
`ff9ds_ark_window_gameover` ROM/ELF). The combat video is
`ds_ark_window_gameover_combat.mp4`; PSX combat counterpart
`psx_ark_window_validation_combat.mp4` has different input/RNG timing.
Native Game Over remains pending. A level-99 host attack/input probe lost,
dismissed Game Over, and survived at the title through 65000 frames
(`pc_ark_attack_probe_level99.log`); it does not validate victory/rewards.

D4768's textured strip emitter now reuses the position table per strip,
preserves alternating scale rows, the conditional UV row mask, context writes,
MAC0 early exit, and OT bucket limits. The strip suite covers both textured
and Gouraud variants in 400 original-MIPS cases with a shared GTE model. Both
targets build. Ark still passes 65000 host frames; Meltigemini advances to
missing DF15C after frame 4000 (`scenarios_textured_strip/report.json`). The
textured-strip addition requires native DS/PSX validation.

## Native Ark defeat and dismissal validation

The fresh DS run reaches Game Over at frame 14419 and returns to title/attract
mode after confirmation, saved at frame 19591. Both states have a zero first
fault marker and no changed bytes in the first 16 KiB of ITCM
(`ds_ark_window_gameover_reached.ml8`, `ds_ark_gameover_dismissed.ml8`, their
inspection JSON, matching `ff9ds_ark_window_gameover` ROM/ELF). Extracted DS
source VRAM matches every original PSX RGB15 pixel; its active bitmap matches
the production scaler replay exactly, with zero coverage rectangles in this
static TIM scene (`ds_gameover_psx_scaler_comparison.json`). This rules out a
source-image/presentation mismatch for this screen.

`ds_ark_defeat_transition.mp4` records Annihilated-to-Game-Over on the DS.
`ds_ark_gameover_dismissal.mp4` and the original GPU-only
`psx_ark_gameover_dismissal.mp4` show the fade to black and return to the title.
The contact sheets were visually reviewed. DS footage uses accelerated wall
time; PSX GPU capture uses emulated time, so these clips do not establish
timing parity. Original PSX has existing memory-card saves; the DS fixture
does not share that memory-card context, so title menu availability/preselection
is not compared here. Both emulators are closed and the normal DS SD config
is restored after validation.

The DS Ark interval from frames 3083 to 9751 takes 65012 emulated VBlanks:
about 6.15 game frames/s at nominal 60 VBlanks/s
(`ds_ark_window_combat_rate.json`). This includes pacing and rendering, not
only raster work, and remains too slow. Victory/rewards, later content,
remaining renderers and overall visual/performance correctness are incomplete.

DF15C now interpolates morph-model vertices, composes each bone with the camera
matrix, and projects the original in-place triples. The final speculative GTE
vector loads and rounded-up vertex-tail writes are preserved. Three hundred
randomized cases compare both vertex arrays and GTE state against original
MIPS using the production GTE model. Only C4's upper padding bits are normalized:
original assembly loads uninitialized matrix-stack padding there, while commands
ignore it and CFC2 sign-extends the low 16-bit matrix element. This is not a
hardware GTE verification. Both target builds pass. Ark and Meltigemini now
complete 65000 host frames (`scenarios_morph_projection/report.json`); DF15C still
requires native DS and original PSX morph-animation validation.

## Optional native timing diagnostics

Build with `PROFILE=1` to use DS timers 2/3 as a free-running bus/64 clock.
Ordinary builds neither configure these timers nor insert timing calls. A
published `port_profile_stats` block covers 128 presentation intervals within
one game mode. Decode a fresh matching state with:

```sh
python3 tools/inspect_ds_profile.py --elf MATCHING.elf CAPTURE.ml8
```

The diagnostic measures GP0 processing, GTE commands, effect callbacks,
display scaling and VBlank waiting. Categories overlap (native effect exports
can call GTE), so percentages must not be summed. It also adds measurement
overhead and is not a substitute for an uninstrumented rate check.

The first fresh Ark sample ends at frame 1377: 142.08 ms/interval, including
91.41 ms GPU (64.3%), 11.65 ms GTE (8.2%), 16.12 ms scaling (11.3%),
10.15 ms VBlank waiting (7.1%), and no effect callbacks in that window
(`ds_ark_timing_probe_first.json`, matching state and `ff9ds_ark_timing_probe`
ROM/ELF). Rasterization is the largest measured cost; primitive-level timing
and later attack windows still need investigation.

Primitive-level profiling publishes a matching `port_profile_draw_stats` block
for polygons, rectangles and lines. At the 128-frame window ending 1633, the
baseline records 67.79 ms polygons and 23.55 ms rectangles within a 147.82 ms
interval (`ds_ark_raster_timing_first.json`). These raster times are included
in GPU command time, not additional independent categories.

The covered-row-span raster path now intersects each row with the same biased
triangle half-planes, skips uncovered interpolant steps with equivalent wrapping
32-bit arithmetic, and preserves all pixel/shading logic. All 4096 randomized
full-VRAM cases match the generic path byte-for-byte. Both targets build;
Ark/Meltigemini each pass 65000 host frames. A fresh matching DS window ending
1633 has identical call counts and drops polygon time to 55.85 ms (17.6% less),
with total interval 136.33 ms (7.8% less). Active bitmap comparison at frame
1663 reports zero changed pixels (`ark_spans_frame1663_comparison.json`,
baseline `ff9ds_ark_raster_timing`, candidate `ff9ds_ark_spans_frame1663`).
This is instrumented timing; uninstrumented combat rate and later animations
remain to be validated.

`CAPTURE_FRAME=N` makes an explicitly diagnostic DS build stop game execution
after presenting exactly that frame, while VBlank/GUI state capture remains
available. It is terminal until the emulator resets. The timing block is
published before the capture stop, so subsequent idle time must not be used
for frame-rate calculations. Ordinary builds omit this stop entirely.

Original Meltigemini was entered and visually confirmed on PSX
(`psx_meltigemini_until_current.png`). An attempt to break specifically at
DF15C produced an internal Redux event-listener exception and unresponsive
controls; that instance was closed. This does not validate morph animation
parity or establish a game/port crash.

The scaler now groups four nearest samples (five source pixels) into paired
bitmap stores when aligned and not crossing the source-row wrap. The scalar
fallback preserves odd widths, wrap and two-byte alignment. Glyph rectangles
mark coverage a word at a time rather than updating every pixel's bit. The
coverage filter and source samples are unchanged. Added tests cover paired
stores, unaligned fallback, odd widths, wrap, word boundaries and overlapping
current/previous glyph rectangles against independent nearest/full-area results.
Both target builds and display regressions pass.

At the same 128-frame Ark window ending 1633, scaler time drops from 16.12 to
9.86 ms and interval time from 136.33 to 124.97 ms after the span change.
Against the original 147.82 ms interval, the combined reduction is about 15.5%.
These are diagnostic measurements with overhead and VBlank quantization, not
an uninstrumented full-battle rate claim. Both source RGB15 and active bitmap
comparisons at frame 1663 are exact (`ark_scaler_frame1663_quality.json`).
`tools/compare_ds_display.py` provides this comparison reproducibly from two
states and matching ELFs, retaining frame numbers and viewport metadata.

Fresh uninstrumented DS Meltigemini with morph projection and covered spans
reaches frames 4056 and 6557, with zero first-fault markers and no changes in
the first 16 KiB of ITCM (`ds_meltigemini_morph_spans_current/late.ml8` and
inspection JSON, matching `ff9ds_meltigemini_morph_spans` ROM/ELF).
`ds_meltigemini_morph_spans_effects.mp4` records actual effect animation.
This validates progress past the previous native routine gap on this route,
not all morph effects, PSX phase/timing parity, victory, or the full encounter.

Opaque cached-indexed sprites and raw/neutral direct-colour sprites now have
a dedicated row path. It preserves live texture reads, CLUT feedback fallback,
transparent/STP black, texture windows, clipped UV origins and mask writes.
The 4096-case full-VRAM raster suite plus 36 focused opaque-feedback cases pass;
both target builds and 65000-frame Ark/Meltigemini host runs pass. Matching
native frame 1663 still has zero source/bitmap differences
(`ark_opaque_frame1663_quality.json`). Rectangle cost drops 23.67 to 9.99 ms;
total sampled interval is 116.61 ms versus the original 147.82 ms (about 21.1%
less). Frame-time savings differ from summed CPU phase savings because VBlank
waiting is quantized. Full uninstrumented combat-rate validation is pending.
