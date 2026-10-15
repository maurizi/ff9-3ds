# Porting notes: PS1 behaviours the decomp's C relies on

Matching C reproduces the PS1 binary exactly, so it also reproduces things that only work because the code runs on
a MIPS R3000 with PS1 memory. Source audits and native runtime probes identify these dependencies. Each class below has a general fix, and an
upstream request where the decomp can make the C say what it means without changing the PS1 output.

| class | example | why it works on the PS1 | port fix | upstream |
|---|---|---|---|---|
| `void` function whose result is used | `D_80067948 = func_800AD44C(...)` | `$v0` still holds the last callee's result | real return type | fixed at the source (decomp `tools/void_returns.py`, 15ec54c) |
| argument passed through in a register | `func_800AA3F4(void)` calls `func_800AAEF8()` | `$a0` still holds the caller's argument | the port defines `REG_PASSTHROUGH(x)` as `x` | fixed at the source (decomp `tools/arg_check.py`, 1b41477) |
| pointer-ness tested by sign | `if ((s32)g >= 0)` = "still a file offset" | RAM pointers are 0x80xxxxxx, negative | the port defines `IS_OFFSET(x)` as "not a pointer into psx_ram" | `IS_OFFSET()` at all 16 sites (decomp 386d31f) |
| pointer-ness tested against 0x80000000 | `if (p > 0x80000000)` | same | `PORT_IS_RAM_PTR()` (patch) | listed in R4 |
| read/write through NULL | AKAO reads, movement collision reads, optional battle texture-animation controls | low RAM is readable and writable | typed, single-evaluation `PSX_NZ()` at reviewed dereferences | annotate original behaviour; retain matching code |
| constant passed as a pointer | `CdControlB(CdlSetmode, (u8 *)0x80, 0)` | reads physical address 0x80 | `PSX_PTR()` in the library call | noted in R4 |
| pointer through a 26-bit script value | `o = (Unk8004AB64 *)func_80052D40()` | 0x00xxxxxx is the RAM mirror | `PORT_SCRIPT_PTR()` (patch), `SCRIPT_PTR_RESTORE` in src_equiv | done for src_equiv |
| busy-wait on the vblank counter | `while (VSync(-1) - t < n)` (`func_800B2C80`) | the counter moves in real time | a run of `VSync(-1)` polls lets a frame pass | none needed |
| interrupt-driven progress | the movie frame counter, CD sectors, AKAO ticks | hardware interrupts | virtual interrupts at VSync and in libcd (`port/psx/irq.c`, `cd.c`) | none needed |

The decomp checks the first two classes from the asm (`tools/void_returns.py`, `tools/arg_check.py`; both report 0),
so new decomp work doesn't reintroduce them. Patches live in `config/patches.txt` as exact text with an expected count, so one that no longer applies fails the
build instead of silently doing nothing. Remove a patch once the decomp fixes its site.

## Debugging recipe (PC build)
```
tools/docker.sh make -j16 pc
tools/docker.sh build/pc/ff9ds --frames 20000 --status 1000 --watch 800AB3C8:1 \
    --press CROSS@2500 --press UP@2900+3 --press CROSS@3000+3      # title -> New Game -> field 70
tools/docker.sh tools/pcdebug.sh <same args>                       # crash: fault address, line, backtrace
```
Most crashes so far were found by following the bad value back with gdb watchpoints (`watch -l`) on PS1 globals,
which are ordinary symbols in the port.

## Running the DS build in an emulator
- **melonDS, DSi mode** (Flatpak `net.kuribo64.melonDS`), the main target: `flatpak run net.kuribo64.melonDS
  build/ff9ds.nds`. It needs your own DSi dumps (ARM9i/ARM7i BIOS, DSi firmware, NAND) and DS BIOS/firmware, kept
  outside the repo (`~/.var/app/net.kuribo64.melonDS/data/melonDS-dsi/`). Config:
  `~/.var/app/net.kuribo64.melonDS/config/melonDS/melonDS.toml` with `Emu.ConsoleType = 1`,
  `Emu.ExternalBIOSEnable = true`, `Emu.DirectBoot = true`, the `[DS]`/`[DSi]` paths, and `[DSi.SD]` with folder sync
  onto `build/sdroot/` (which holds `ff9ds/raw/FF9 CD1.img`, a link to the disc image). The game reads `sd:/ff9ds`.
  Verified 2026-10-11: the title -> New Game script reaches field 70 (mode 1, ovl_0A) at frame ~5000.
- melonDS doesn't exit on SIGTERM: close it with `pkill -9 -x melonDS` after a run (never `pkill -f`, which matches
  its own shell). Use dotted keys under `[Gdb]` if you set any: melonDS can't save `[Gdb]` + `[Gdb.ARM9]` table pairs.
  The Flathub build has no GDB stub.
- **melonDS, DS mode (4 MB)**: runs the game logic without the VRAM/SPU shadows, only up to the first overlay the
  game restores from its VRAM cache (after New Game). DLDI folder sync (`[DLDI]`) gives it the SD card.
- **no$gba 2.8 under Wine**, fallback with a debugger: `tools/nogba.sh setup DIR` (copies your no$gba DSi setup and
  builds a FAT32 `DSI-1.SD` with the disc image), `tools/nogba.sh run`. It boots the DSi menu from the NAND and
  starts the cartridge with an XTest key press (`tools/xkey.py`).
- DeSmuME: no DSi mode, and its R4 slot-1 emulation stalls on this ROM; not used.
- For manual validation, this workstation’s melonDS keyboard bindings are arrows, Z (DS A / PSX Cross),
  X (DS B / PSX Circle), Return (Start), A/S (X/Y) and Q/W (L/R). A fixed-frame input script can miss
  title-menu readiness as port timing changes; inspect the cursor and confirm New Game manually.
- `make ds AUTORUN=1 AUTOPRESS="CROSS@2500,UP@2900+3,CROSS@3000+3"` runs the title-to-New-Game script with no input.
  This historical script is timing-sensitive; it did not reliably confirm New Game with the current GX path.
  The status line at the bottom of the console shows frame, mode, overlays and libcd state.

## DS hardware graphics (M2, 2026-10-12)

`port/ds/render_gx.c` consumes the shared GP0 decoder. `make ds` uses GX; `make ds SOFTDISPLAY=1` uses the software
reference GPU. Runtime option changes are tracked by `build/ds/runtime-config`, so switching paths does not
require deleting objects. The GX console reports the last frame's submitted polygons and cumulative dropped
primitives. Empty VBlank submissions retain the last display list.

Full 256×256 pages exhausted texture VRAM at the title and field entry. The cache now converts the used region,
aligned to 16 texels and rounded to power-of-two dimensions, and shares palettes for the same CLUT generation.
Texture-window state is baked into those regions. Current and preceding submissions pin allocations so later
uploads cannot overwrite textures still being rasterised. Failed allocation evicts older unpinned entries;
if the remaining working set cannot fit, the primitive is dropped and counted. Both the 2048-polygon and
6144-vertex hardware limits must be respected (the port reserves 48 polygons).
The GX path now assigns A/B/C/D to textures (512 KiB). The debug console uses bank H
instead of C: its font and tile map fit within 32 KiB. The previous 384 KiB layout
exhausted allocations on the opening name-entry screen. Emulator validation is recorded below.

Cache lookup uses hash chains. Dirty tracking covers each converted region rather than the entire page;
RAM copies of converted pixels/palettes detect unchanged contents inside dirty blocks, avoiding redundant
allocations when neighbouring framebuffer or CLUT entries change. Dialogue clipping is independent of the
framebuffer origin: `PutDrawEnv` selects the target, while subsequent GP0 areas clip sprites and polygons.

VRAM generation tracking wraps at 1024×512 and includes every block touched by an unaligned 256-colour CLUT.
`tools/docker.sh make test-gx` checks wrapping, palette invalidation/sharing, region conversion, nonzero
transparent indices, texture windows, allocation failure, pinned lifetimes, unchanged dirty contents, empty frames,
vertex limits, dialogue/polygon clipping and CPU sprite compositing (subtraction, modulation, STP,
transparent texels and geometry invalidation) under ASan/UBSan. It mocks libnds and does not validate the hardware rasteriser.

Validation so far: PC reference renderer reaches field mode through 6000 scripted frames; melonDS DSi shows the
title/menu and the first room with Zidane. Title frames have zero drops; field videos exposed missing glyphs,
background flicker and repeated texture allocation. Glyph clipping and unchanged-content reuse have been fixed;
field fidelity/performance remain under investigation. M2 remains
in progress: offscreen DRAWENVs are skipped, additive blending and subtractive polygons are approximated,
textured STP is treated at polygon granularity outside the exact sprite path, and the fixed viewport targets 320×224. Battle/world-map scenes,
real hardware and performance still need validation. Game-derived screenshots stay under `build/`.

Tracing the PC software GPU at the first-room dialogue identifies textured subtractive (ABR=2) sprites with
RGB 64,64,64 over the background. A constant black-alpha overlay cannot reproduce PSX subtraction. GX now tracks
known rectangle pixels in a 320×224 CPU shadow, invalidating polygon/line bounds and approximated blends. An ABR=2
rectangle over entirely known pixels is composited with PSX modulation, per-texel STP and saturating channel
subtraction, then drawn as an opaque direct texture. Identical composites share the existing pinned cache.
Uncertain areas retain the approximation; this is not an exact general-purpose blend implementation. Full-frame
fills and each new GX submission initialise the shadow from the GX clear colour. The shadow costs about 210 KiB;
CPU work and texture VRAM use still require real-hardware profiling.
The console’s `blend` line counts exact sprite composites and rectangle fallbacks cumulatively.

The exact sprite path was checked in melonDS at "Sure is dark...": `build/validation/ds_first_room_subtraction.png`
and the eight-second `ds_first_room_subtraction.mp4` show the black room background seen in the verified PSX
capture, with no dropped primitives in that run. `psx_first_dialogue_advance.mp4` and
`ds_first_dialogue_advance.mp4` capture dialogue closure and Zidane moving to "Guess nobody’s here yet...".
The later stills are `psx_dialogue_advanced.png` and `ds_dialogue_advanced_current.png`; both show a small lit area
around Zidane. The clips expose different transition timing, so they are not a lockstep timing match. Geometry
blend fallback and character shading still need closer comparison. The PSX state `ds_port_first_room_verified`
is saved in the decomp’s portable Redux directory for repeatable checks.

## Comparing with the original PSX game

Use the original game in PCSX-Redux via `../ff9-decomp/tools/emu.py`, as well as the port's PC software renderer.
Record the disc, memory-card contents, selected menu entry, scene/field ID and input sequence. The installed Redux
card 1 has six saves and preselects Continue. A port with no card saves can preselect New Game, so a fixed Up/Cross
sequence is not a portable comparison recipe. Inspect the selected entry before confirming it.

Verify actual field entry, not just a movie or a resident overlay. On 2026-10-12 a live PSX boot reached field 70
(GameState.mode=1, GameData.fieldId=70) and the visible "Sure is dark..." dialogue. Its screenshot is
`build/validation/psx_first_field.png`; the room is substantially darker than in the DS capture. This single
capture does not establish that the background should be completely black throughout the scene. Compare
matched animation frames before making that claim. Older title save-state input did not behave like the live boot;
use the verified live scene for this comparison. Port FMVs are currently two-frame placeholders, so absolute
frame numbers and movie durations cannot be matched to the PSX.

Capture short videos for fades, transitions, cursor movement and character animation. Commands:
```
python3 tools/capture.py --window melonDS --seconds 8 --out build/validation/ds_scene.mp4
python3 tools/psx_clip.py --seconds 4 --out build/validation/psx_scene.mp4
```
The first records one X window at 30 fps in wall-clock time (including the DS debug console). The second requires
paused Redux and advances two PSX VBlanks per frame, capturing directly from the GPU at 320×224 without desktop
debug windows. It leaves Redux paused. Do not issue emulator commands concurrently with the PSX clip recorder.
`build/validation/psx_first_room_idle.mp4` is a verified four-second, 120-frame baseline. Compare scene milestones,
occlusion, lighting, text, actor poses and motion; retain differences in notes instead of declaring success from
a single favourable frame.

## Candle sequence validation (2026-10-12)

Moving toward the opening room’s candle exposed a NULL collision-result read in the draft
`func_800475D8`: PC SIGSEGV at 0x0F, DS data abort. The original PSX assembly reads `0xF(s3)`
at 0x800478D8 before its NULL check at 0x800478FC. The counted patch in `config/patches.txt`
maps just that read through `PSX_NZ(o)`; `o` remains NULL for the collision test. The live PSX
byte at 0x0F is zero, matching the port’s low-RAM shadow for this calculation. See decomp finding F2.

Reference states in the portable Redux directory: `ds_port_before_candle` (second dialogue),
`ds_port_candle_ready` (question bubble beside the candle), and `ds_port_light_candle_choice`
(the "Light the candle / Cancel" choice). These are reached from a verified live New Game boot.

The current viewport reduces 320×224 PSX pixels to 256×179 DS pixels. Existing glyph sprites
are scaled with the scene, losing thin strokes; this is separate from missing primitives
caused by texture allocation or clipping. Native UI/font layout remains the planned M5 work
in `dual-screen.md`. Enlarging melonDS does not restore the lost glyph pixels.

The 512 KiB GX build was booted fresh in melonDS (old savestates contain old ARM code
and cannot validate a rebuilt renderer). New Game selection was inspected explicitly.
The opening candle choice, lighting animation and name entry all retained a zero cumulative
drop count, unlike the preceding 384 KiB build's 16,869 drops at name entry.
`build/validation/ds_bank_fix_candle_choice.mp4`, `ds_bank_fix_candle_animation.mp4`
and `ds_bank_fix_name.png` record these milestones. The PSX name reference is
`psx_name_reference.png` from `ds_port_name_entry`. The HELP/NAME labels look clipped in GX despite zero drops. The PC software renderer and
PSX show full labels. A trace shows ordinary 7×7 sprites inside full-screen clipping bounds;
the saved DS cache contains complete HELP glyph pixels (`ds_help_cache_pixels.png`).
This points to lost single-pixel strokes during viewport downscaling rather than missing
source glyphs. Native font/UI rendering is still needed for readable small labels.
The dark-room rectangular shading artifact also persists around Zidane.

Name confirmation preserves "Zidane" and reaches the fully lit "It’s me, Zidane!" greeting.
`ds_candle_room_reveal_gx512.mp4` captures name confirmation and the return to the room;
`ds_candle_greeting_gx512.mp4` and `ds_candle_greeting_gx512.png` show the completed dialogue.
The matching original reference is `psx_candle_room_reveal.mp4` and
`psx_candle_room_revealed.png`, with state `ds_port_room_revealed`. The DS run keeps zero drops.
These compare sequence milestones, not identical timing or actor coordinates: DS animation
runs slowly in wall-clock captures, and the manually aligned interaction position differs slightly.
Candle/background placement agrees; character shading and the dark-room rectangular blend
artifact remain differences. Name entry uses untouched text rather than editing the game’s name.

## First battle exploration (2026-10-12)
The original PSX run was advanced from a verified New Game through candle lighting,
name confirmation and the Tantalus dialogue into battle 336 against masked Baku.
Reference states: `ds_port_tantalus_greeting`, `ds_port_baku_closeup`,
`ds_port_first_battle`, and `ds_port_first_battle_menu`. Animation references are
`build/validation/psx_baku_entrance.mp4` and `psx_first_battle_attack.mp4`.
The matching DS entrance captures exposed texture allocation failures and a cached
battle-overlay relocation failure, rather than an attract-movie transition.

Cached overlay prefixes restored by `func_80012900` now call `psx_overlay_patch`
after either the RAM copy or `StoreImage`; sectors not in that prefix are still
patched by the CD layer. See `trace_battle_loader.log` for the pointer watchpoint.
Battle initialization also submits an OT and packets inside one native stack
buffer. `port_draw_ot_buffer` receives that buffer's exact bounds at the counted D0940
call site. Links within those bounds use the head's native address window; other
links resolve into the PSX arena. Plain `DrawOTag` requires arena links. This
avoids an intermittent ASLR collision where a native link's low 24 bits also
address arena RAM. The regression probe forces this collision deterministically;
it also covers native-to-arena chains.

Nine battle scalar helpers, the background emitter/projector, the skeletal pose
builder and the eight-format model packet emitter have native implementations.
Three initialization reads/writes through NULL are patched at counted sites; their
original PSX values and the limits of the low-RAM shadow are documented in F4.
These changes advance execution into battle drawing; combat is still being traced.

Validation commands:
```
tools/docker.sh make test-gpu test-gx test-battle
tools/docker.sh make pc-gx
```
`test-gpu` exercises real 32-bit native/arena OT chains under ASan/UBSan.
`test-battle` compares 2700 scalar-helper cases against the original instructions;
160 background cases, 100 pose cases, 160 mesh packet cases, 1000 orientation/
interpolation cases, 400 particle-motion cases and 200 effect rectangle/sprite
cases compare CPU logic
against those instructions using shared deterministic GTE/rotation models.
The latter tests validate packing, culling, sorting, hierarchy and register paths;
they do not independently validate GTE precision, trigonometry or DS rasterisation.
Original instruction bytes are read from the user's generated decomp assembly at
runtime, rather than embedded in tests.

`build/tests/gx_host.elf` runs the real game through the production GX translator
with a contiguous 512 KiB texture allocator model. It prints texture versus geometry
budget failures and allocation pressure. It does not rasterise or model palette
exhaustion, physical bank remapping or DS timing. The enclosing-region cache avoids
duplicate uploads for smaller faces, including unchanged rows in dirty parents;
subregion reuse never marks other dirty parent pixels clean. Actual DS captures
remain necessary. Bottom-screen diagnostics report separate texture/budget counters.

The native effect path now includes E3EEC frame orchestration, five orientation/
interpolation helpers, both DFE4C/DFE54 particle integration entries, and
D4478/D40CC effect rectangle/sprite emitters. Native code uses its platform stack;
E3EEC's scratchpad-stack switches and unused saved-SP bookkeeping are omitted.
A counted E4A3C draft patch reconciles the MIPS stack-home struct with its native
four-argument callers. Battle help-text measurement uses a native ABI adapter
for the explicit MIPS hidden-return call (F5/F6 in `decomp-requests.md`).

`python3 tools/battle_smoke.py` drives a reproducible PC New Game through the
opening and Baku transition; pass `build/tests/gx_host.elf` for allocator tracing.
The script sends inputs at game frame counts, not wall-clock times. It is a
runtime smoke probe and cannot replace visual or interactive combat validation.

Fresh-build room checks: `psx_first_room_latest.png` agrees with the DS shot
`ds_interpolation_run.png` on the dark background and character spotlight.
`ds_battle_build_candle_motion.mp4` actually captures idle motion before the candle
approach, despite its filename; its contact sheet must not be described as candle
lighting. `psx_candle_choice_motion_latest.mp4` captures the PSX reference after
confirming the candle choice. DS wall-clock clips and PSX emulated-time clips have
different time bases; compare animation poses/transitions rather than durations.

The PC path gets through battle execution to mode 8 after the text ABI adapter.
That exposed a stale `func_801EF284` dispatcher name for overlay 65's existing
`battleEnd_main`; a counted call patch plus an explicit cross-overlay pin uses
the current name. `tools/pcdebug.sh` now retains `[frame ...]` status lines instead
of dropping all bracketed output, so progression can be reviewed in logs.
The DS loaded image is identified separately from the on-disk ROM when builds
continue during a run; captures from an older loaded image do not validate later
battle helpers. The software-display build also compiles successfully.

Latest full PC smoke: `pc_battle_end_progress.log` completes 60,000 frames normally.
Frames 24,000–26,000 run mode 2; frame 28,000 returns to field mode 1 with overlay
65 loaded. `pc_battle_frame_030000.png` and `pc_battle_frame_035000.png` show the
Tantalus briefing with Baku unmasked. This establishes story execution through
that encounter, not battle graphics completion: the 24,000–26,000 software dumps
are mostly black with a thin diagonal textured wedge (25,000 has the "Oww!" text).
They do not match `psx_battle_menu_latest.png`. A first-background transform probe
(`pc_battle_projection.log`) preserves vertex data and live GTE controls for the
next comparison. Real GTE/packet/viewport tracing is still required.
The longer GX host path also exposes renewed texture failures and geometry-budget
failures after the opening. Zero drops in the candle scene must not be generalized
to battle or later multi-character fields.

Follow-up visual issue (user priority after battle): rectangular/blocky artifacts
around Zidane are visible in the opening room, especially during expansion of the
candle light (`ds_current_reveal_progress.png`). Compare the moving character and
light edge against PSX clips after fixing the mostly black battle output. This
is a recorded rendering defect, not an expected result of screen scaling.

The final longer GX allocator run (`gx_battle_end_progress.log`) completes 60,000
frames and reaches the briefing, but reports 15,608 texture-allocation failures
and 75,312 geometry-budget failures cumulatively. These are the contiguous host
allocator model's counts, not measured DS hardware drops. The 60-second DS clip
`ds_latest_candle_reveal.mp4` records the growing pool of light and the rectangular
patches around Zidane; its contact sheet is `ds_latest_candle_reveal_contact.png`.

After the explicit native-OT bounds fix, `final_battle_scoped_checks.log` passes
DS/PC builds plus GPU, GX and all 4,720 battle equivalence cases.
`pc_battle_scoped_smoke.log` independently reruns to 30,000 frames, crosses mode 2
and returns to mode 1 at 28,000 without a fatal stub or stack overwrite. The
upstream decomp checkout remains unchanged.

### Battle camera and geometry shadow follow-up

The mostly black battle was caused by the port's GTE OP implementation: it stored
IR1 before calculating IR2 and IR3, which must all use the original input vector.
The camera consequently had two nearly antiparallel axes. Snapshotting the three
inputs fixes the camera basis. `pc_gte_cross_product_smoke.log` reaches the briefing
normally; `pc_battle_op_fixed_025000.png` and `pc_battle_op_fixed_026000.png` now show
the room, all four party members and masked Baku. DS battle validation is pending.

`make test-gte` checks the production GTE core with independent golden cases under
ASan/UBSan. `tools/test_gte_psx.py` additionally compares 32 OP vectors, including
shift, limit and saturation cases, with live PCSX Redux registers. It saves and
restores the emulator state around synthetic instructions; all cases pass.

The rectangular candle artifacts have a renderer fix under DS validation: model
polygons now rasterise into the CPU compositing shadow, preserving their actual
coverage and colours instead of invalidating a bounding rectangle. Subsequent
subtractive light sprites can therefore compose over the model. The software
rasteriser clamps interpolated channels before packing, preventing a negative
rounding result from causing undefined shifts. GX regression checks pass.

Consecutive opaque textured rectangles merge only when screen positions, UVs,
draw state and colours form one continuous rectangle. Pending rectangles retain
their draw environment and flush before geometry, frame submission and VRAM
writes. A regression verifies that a queued sprite samples its original texture
before a subsequent upload changes it. An ARM/O3/TCM experiment produced a black
field on DS, including with TCM and rectangle merging individually disabled.
Returning the GTE and rasteriser to Thumb/O2 restores Zidane and the opening
dialogue. The ARM flags are removed; no speed improvement is claimed. The compiled
ARM946 GTE independently passes the 32 PSX OP vectors, so that check alone does
not explain the regression. Signed shifts of negative GTE intermediates have
also been replaced with multiplication to keep that arithmetic defined.

`gx_merge_shadow_progress.log` completes 60,000 frames and reaches the briefing,
but reports 454,121 texture failures and 73,907 budget failures. The corrected
camera now exposes substantially more battle geometry; these numbers are not a
direct performance comparison with the old broken camera. A further lossless
texture optimisation stores composited tiles with at most sixteen exact colours
as four-bit indexed textures, sharing identical palettes. More colourful tiles
retain direct colour. Tests decode the result to verify every pixel, including
opaque black, transparent padding and the seventeen-colour fallback. Its full
allocator run and DS validation remain in progress.


### DS image-quality rollback (2026-10-12)

The geometry-shadow replay and indexed-composite experiments regress the actual
DS opening: the growing candle light acquires a stepped edge even before upload
failures, and later texture pressure loses parts of the scene. Host pixel checks
are insufficient to approve those changes. The default renderer again invalidates
polygon bounds in the sprite shadow, uploads composited tiles as direct RGB555,
and submits rectangles individually. It uses the original containing-region
preference and 64 KiB palette bank. These experiments require
`GXEXPERIMENTAL=1`; they must pass PSX screenshot and animation comparisons on DS
before becoming defaults. The GTE cross-product and defined-arithmetic fixes remain.
The older Zidane bounding-box artifact and texture-pressure issues remain open.


### Texture-pressure investigation (2026-10-12, continuing)

The first direct-colour failure in the opening-to-battle allocator probe pins
63 regions (446,464 bytes), but those regions contain only three distinct images
(16,384 bytes). Direct images now share exact converted pixels across source
regions, while binding retains each request's UV origin. Indexed page lookups
retain the previous array-order preference rather than changing enclosing-region
selection. Host tests cover shared images at different UV origins and a later
source write without overwriting an earlier submitted texture. The corresponding
DS build retains the round light pool; the old character bounding-box artifact
remains visible and its candle animation is being captured.

That sharing alone completes the 60,000-frame probe with 377,605 texture failures
and 73,982 geometry-budget failures. A frame-wide primitive collector is now
behind `GXPLAN=1`: it retains drawing state/framebuffer origin, chooses enclosing
regions before upload, and flushes before writes that intersect queued texture pages or CLUTs. Its current probe completes
60,000 frames with 310,869 texture failures and 78,450 budget failures. This is
still inadequate and has not been approved as the default DS renderer. The native
software path and DS/PC builds still pass; passing host allocator checks does not
establish image quality or emulator timing.


Indexed source-pixel sharing is additionally behind `GXSHARE=1`: immutable
pixel images have separate palette handles, and binding assigns the logical
palette to the shared GPU image. Tests draw the same indices through red and
blue CLUTs and release each handle while retaining the other image reference.
This does not enable the earlier indexed-composite or geometry-shadow experiments.
Direct framebuffer regions are excluded from atlas planning because their extra
padding increases memory pressure. Zero-area faces are rejected before upload,
matching the software PSX rasteriser. The allocator model now uses the default
64 KiB palette bank; earlier 96 KiB runs must not be treated as that configuration.


`gx_indexed_plan_progress.log` (before tighter UV origins) completes the same
60,000-frame path with 294,797 texture failures and 76,069 budget failures. The
64 KiB palette model reports no palette failures. This remains far from the goal.
A further `GXPLAN=1` change reduces padding by using exact UV minima (even minima
for four-bit packed pixels), while retaining 16-pixel minimum allocation sizes.
Dirty-generation sampling now covers partial eight-texel units; brute-force
checks include unaligned UV origins. This variant is awaiting its allocator and
DS comparisons. The default ROM is rebuilt without experimental flags after each
preserved diagnostic ROM.

The tighter-origin probe regressed badly and is no longer selected by `GXPLAN`.
At its first failed source upload (submission 9066), 539 cache entries were pinned,
192,256 bytes of pixel RAM were occupied, and 332,032 bytes were free. The largest
contiguous hole was only 19,712 bytes: an optional 256x128 indexed atlas needed
32,768 bytes for a face whose own UV region fits in 32x16. Atlas upload failure
now disables that atlas for the batch and retries the individual face region;
previously it dropped the face outright. A forced-allocation-failure fixture
checks the fallback, texture origin, and absence of a reported drop. This remains
an experimental planner change until DS visual validation.

The allocator probe's texture-name table now has 16,384 slots rather than 4,096;
libnds allocates texture names dynamically, so the former artificial limit could
reject allocations prematurely. Pixel RAM and palette RAM remain 512 KiB/64 KiB.
The production logical cache remains at 2,048 entries; enlarging it did not fix
the observed fragmentation failure.

Ten ARM9 PC samples from matched melonDS savestates landed in sprite sampling /
compositing (four samples), texture comparisons (three), libc memcmp (two), and
memset (one). These are a small sample, not a frame-rate measurement. The renderer
now compares aligned pixel data in words, hashes aligned words, and shades opaque
indexed sprites once per palette entry before sampling. ASan/UBSan fixtures cover
aligned/unaligned comparisons and independently computed modulation, transparent
texels, and opaque black in both four-bit and eight-bit palettes. DS/PC default
builds pass; actual DS performance and later-scene correctness still need checking.

The candle confirmation was captured from a saved choice screen, with capture
started before Cross, in `ds_indexed_plan_candle_confirm_to_reveal.mp4` and
`psx_candle_confirm_to_reveal_reference.mp4`. Contact sheets confirm rectangular
lighting artifacts around the DS character versus the PSX's smooth reveal.
Earlier clips labelled growth/expansion started after expansion or at the idle
choice and should not be used to assess the candle action.

Dirty texture blocks are now 64 words by eight rows, with separate 64-word
per-row CLUT generations. Tests prove writes at source row 224 do not invalidate
sampled rows 192–207, and that a write to CLUT row 301 leaves a palette at row 300
valid. Wrapped writes and brute-force generation checks still pass. The matched
DS diagnostic ROM/ELF is `ff9ds_cpu_precise`, booted fresh; old states would restore
old executable code. Its first dark-room dialogue reports zero dropped primitives.
The default ROM is built with `GXPLAN`, `GXSHARE`, and `GXMERGE` unset.

`GXMERGE=1` now enables only adjacent opaque textured rectangle merging, without
`GXEXPERIMENTAL`'s generated indexed composites or geometry shadow replay. Merging
retains the buffered primitive's framebuffer origin. A third ASan/UBSan build tests
this isolated configuration; host pressure and real DS image comparisons remain
required before considering it for the default.

The workstation's melonDS JIT was disabled. It is now enabled for the validation
run; the previous configuration is saved as `build/validation/melonDS_before_jit.toml`.
This does not establish target hardware performance. Only one melonDS instance is
kept open, and matched ROM/ELF/savestate sets are preserved before replacing it.

### DS battle validation, sound pointer crash, and UI insertion

The matched GX DS ROM `ff9ds_cpu_precise` verifies the GTE OP camera repair on
DS, with the room and all five characters visible. Large missing-texture patches
remain in battle. A real DS run then stopped with `SystemError('S', 310)`: sound
panning `func_800E976C` interpreted a native stack SVECTOR as a unit index because
the original PSX pointer-bit test does not recognise ARM stack addresses. A
counted patch distinguishes small IDs from native pointers using `PSX_RAM_SIZE`.
`tools/test_battle_pan.py` checks all 18 IDs, the -1 sentinel, and native static,
stack and DS-address pointers with the actual patched function and software GTE.
`ff9ds_battle_pan_fixed` passes the former crash during DS battle actions.

The DS software comparison path shows an intact battle room and removes the
rectangular candle-light patches seen in GX. Captures include
`ds_software_battle_entry.mp4`, `ds_software_battle_menu_compare.png`, and
`psx_first_battle_for_software_comparison.mp4`. These comparisons also exposed
missing battle UI in both port backends. `ContinueDraw` was empty, discarding the
VBlank UI ordering table even though `BreakDraw` correctly returned zero for an
idle synchronous renderer. It now executes the inserted list. `GetDrawEnv2`
snapshots the current GP0 registers, as the original PsyQ assembly does, so the
UI list can restore clipping, offset, texture/window and mask state afterward.
The GPU fixture checks idle insertion, a following continuation, and all six
register snapshot words. `pc_battle_ui_25000.png` shows command/status panels;
the 30,000-frame PC opening run returns to the briefing without a fatal.

Use `tools/save_ds_state.py` for slot-8 captures: saving via XTest is asynchronous.
Immediately copying the slot can preserve an older state. The helper waits for a
fresh complete write and reports the game frame and guest VBlank count from the
matching ELF. The former `ds_cpu_precise_first_battle.ml8` is actually a stale
frame-3396 name-screen state and must not be used as battle evidence. Guest
VBlank deltas permit FPS measurements despite accelerated emulator wall time.

Standalone GX rectangle merging did not reduce the 60,000-frame host pressure
probe: 272,004 texture failures and 76,069 budget failures with or without it.
Cross-CLUT planning regressed allocation pressure and was reverted. Both remain
outside the default. Hardware rendering is still incomplete.

`ff9ds_software_ui` is a fresh DS boot with the UI insertion and flat-colour
interpolation optimisation (before palette pre-shading). Its command/status
panels match the PSX layout in `ds_software_ui_battle_verified.png`. The actual
candle choice/action/reveal is captured in `ds_software_ui_candle_action.mp4`;
its contact sheet shows the restored light without GX's rectangles.
First-battle guest-time measurement from frame 6095 to 7644: 1549 game frames
over 13691 emulated VBlanks, approximately 6.77 FPS. This remains a serious
performance issue, despite unlimited emulator pacing and JIT.

Software rendering is now the default; `SOFTDISPLAY=0` explicitly selects GX.
The flag now compares against 1, so 0 no longer accidentally enables software.
The software pixel loop skips RGB gradients for flat polygons and pre-shades
large flat indexed primitives once per CLUT entry. Original texels are retained
for transparent-zero and STP decisions, and caching is disabled when writes
could modify the CLUT. Texture-index reads remain live. `make test-raster`
compares full VRAM against the generic path for 4096 random mixed primitives,
plus wrapped palette-feedback, opaque-black and shadow mask/known-pixel cases.
ASan/UBSan and all three GX fixtures pass. A host O2 flat-indexed-quad benchmark
measured 0.577s generic versus 0.421s optimised (1.37x); it is not a DS FPS result.
Interpolant stepping now uses defined unsigned 32-bit addition to retain register
wrapping outside thin triangles without C signed-overflow UB.

`make raster-ds` builds a disc-free ARM9 timing ROM using the production renderer
and generic oracle, comparing Thumb and ARM implementations with identical
VRAM and checking resulting pixels. Guest timer counts, not wall time, are used.

The disc-free ARM9 timing probe also checks solid blended polygons and rectangle
fills. Pixel outputs match in seven cases. ARM plus instruction-TCM placement
reduces guest timer counts in all seven: flat indexed quad 51.31M -> 27.82M,
solid Gouraud subtract 37.66M -> 27.20M, solid rectangle 21.29M -> 0.90M
(for 64 96x96 draws). Captures: `ds_raster_timing_extended.png`,
`ds_raster_timing_itcm.png`; machine-readable counts are in
`raster_arm9_timing_summary.json`. These are isolated raster measurements, not
whole-game FPS. Forcing texture samples inline helps rectangles but has mixed
polygon results outside TCM; the TCM version improves the tested cases.

Only DS `port/psx/softgpu.c` is now compiled in ARM mode at O2, with triangle,
line and rectangle raster loops in `.itcm` (total game ITCM 11544 bytes). GTE
code and its flags are unchanged. The former ARM/O3/GTE experiment's failed
room test is not reused as validation. `ff9ds_software_fast` is a fresh matched
ROM/ELF boot for this configuration, with automation ending before battle inputs.

The fresh optimised DS run shows an intact battle and UI, then manually completes
the encounter and returns to Baku's briefing. `ds_software_fast_battle_verified.png`
and `ds_software_fast_after_manual_inputs.png` preserve those states; animations
are in `ds_software_fast_battle_entry.mp4` and `ds_software_fast_manual_attack.mp4`.
An idle battle interval measures 3125 game frames / 21193 guest VBlanks = 8.82 FPS
(frame 5831 -> 8956), versus the pre-palette/TCM UI build's 6.77 FPS interval.
Intervals differ in exact enemy animation phase, so this is an approximate
whole-scene comparison, not deterministic per-frame timing. Performance is still
far below the target. DS/PC default and explicit GX builds pass.

The loaded PSX first-battle reference state later proved stuck at VSync+0x24,
continually trying to read root counter 1 twice with equal values. GPU VBlanks
advance but game pad globals remain zero under Cross/Circle holds. Thus the latest
`psx_battle_confirm_attack_reference.mp4` is an idle clip, not attack evidence.
Static battle comparisons remain useful, but animation requires a fresh reference
boot with verified input and game progress. The reference is being cold-restarted.

### Fresh PSX reference and post-briefing execution failure (2026-10-12)

DuckStation keyboard bindings were empty; applying automatic keyboard mapping gave Cross K, Circle L, Start Enter, and arrow directions. New Game was explicitly selected and captured (`duck_newgame_cursor_verified.png`), followed by the storm opening and playable Zidane dark room. `duck_candle_action_reference.mp4` captures confirmation of Light the candle, the character motion, room reveal, and name screen; its contact sheet confirms animated progress. Window capture is wall-clock time, not lockstep game-frame timing. This replaces the stalled Redux attack attempt as a working reference for future animation checks.

The optimized software DS route completed the battle and briefing quiz (Princess Garnet selected), then stopped at a black transition. Two matched savestates report game frame 52369 unchanged while guest VBlanks increased 469864 to 479065. The ARM9 state has PC 0xFFFFFFFE and CPSR 0x024900F8; CP15 control is 0x78 instead of the prior running state’s 0x5307D. The cause is unresolved. An interpreter replay from the matching pre-briefing state is underway; do not count this route as passing past the briefing.

GUI key streams must run sequentially: XTest keys target the currently focused window, so manipulating a second emulator while a key script is running can steal its input focus. Passive window recording may run concurrently.

The interpreter replay reproduced a data abort at the full-screen TIM routine `func_800B4A5C` after selecting Princess Garnet. The correctly routed PC smoke test faults at WIP line 131 as well. Both CPU-side uses of the stored OT[8] link now pass through `PSX_OT_PTR`, restoring the arena high byte without adding the PSX arena base a second time. Original MIPS confirms that following the stored link is intentional. The GPU fixture checks the linked-bucket insertions and packet order under ASan/UBSan. Fresh DS and PC route validation is pending; old savestates contain the old executable and cannot validate this fix.

The first fix lets the PC pass the full-screen TIM loop, exposing a second crash at evaluator opcode 0x61 (distance to an object). Its popped 26-bit object pointer 0x02077F04 becomes 0xFE077F04 after signed expansion; the previous identity `SCRIPT_PTR_RESTORE` hook failed to remove that expansion. The hook now delegates to the same 26-bit mask used for event opcodes 0x70/0x71. Native pointers remain within the 0x02xxxxxx arena. Route validation continues with both fixes.

With both pointer fixes, `tools/battle_smoke.py --briefing --frames 60000 --dump 60000` finishes normally and captures Vivi in Alexandria (`pc_postbriefing_both_fixed_60000.png`). This newly added quiz route selects Princess Garnet; the prior smoke route loops on Queen Brahne and does not cover the TIM transition. The fresh DS build `ff9ds_script_pointer_fixed` is undergoing the same route from boot.

The fresh DS validation includes manual early candle/name inputs, so its battle begins earlier than the PC smoke schedule. Consequently the scheduled Start@20000 paused the battle. A matched savestate was copied with only the port’s scripted-pad count reset to zero (`ds_both_fixed_manual_battle_no_script.ml8`, JSON records the one-word change), then restored with the same ROM/ELF. Start unpaused it, and manual controls resume encounter validation. No game state, program bytes, or progress were changed in that diagnostic copy.

The fresh both-fixes DS run now manually completes the battle and reaches the briefing. `ds_both_fixed_manual_attacks.mp4` and its contact sheet show actual player attacks, flashes and enemy movement; this replaces the earlier idle ability-menu clip as attack evidence. The PSX combat clip likewise shows real attacks, but the Alexandria arrival clip begins at the already-reached “Are you awright?” dialogue and only records idle character motion, not the FMV-to-field transition. PC Alexandria exploration to 85000 frames exits normally and captures Vivi in the square; fresh DS quiz-transition validation remains pending.

The fresh `ff9ds_script_pointer_fixed` DS run selects Princess Garnet (`ds_both_fixed_princess_selected.png`) and passes the formerly crashing transition to Vivi’s “Are you awright?” scene (`ds_both_fixed_transition_live.png`). The matched state reports game frame 57160 / guest VBlank 341228. Both pointer fixes are now verified on DS as well as PC. `ds_both_fixed_postquiz_transition.mp4` records the quiz confirmation and transition; no FMV playback is claimed, since the movie override is still headless.

The rasterizer now skips UV setup for untextured polygons and gives untextured Gouraud lighting overlays a loop that only steps RGB. The first layout regressed textured ARM9 cases by 2–5%; putting the flat branch first recovers most of that cost. The reordered disc-free DS probe matches all seven full-VRAM outputs and measures 24.45M ticks for the solid Gouraud subtract case, versus 27.20M previously (10.1% reduction). Flat indexed 27.99M vs 27.82M, textured Gouraud 39.50M vs 39.45M, subtract 31.04M vs 31.28M. Capture/JSON: `ds_raster_uv_reordered_timing.png`, `raster_arm9_uv_reordered_summary.json`. Host ASan/UBSan differential passes. This is isolated raster timing, not a new whole-game FPS measurement.

### Font coverage during DS display scaling (2026-10-13)

Nearest-neighbour 320-to-256 scaling discards thin font strokes. A full-frame area-coverage prototype matches an independent scalar test, including wrapped VRAM, RGB channels and clipping. ARM/ITCM grouping and exact small reciprocal division reduce its 320x224 cost from 1.24M to 0.735M guest ticks/frame, still too expensive. The production candidate filters only cells touched by decoded proportional-font sprites (E100061E, height 13) and small-font sprites (E100061F, 8x8), identified from main.c. It retains font regions across extra VSyncs without drawing. Scene pixels keep nearest sampling, while glyph-border coverage preserves their strokes. The bitmap is pre-scaled and the DS affine unit runs at 1:1; display-size changes clear old pixels and update centering.

Host ASan/UBSan tests check known strokes, independent channel sums, wrapping, cropped borders, selective-vs-full coverage, empty selection and the decoder’s font-page/size/clipping decisions. The disc-free ARM9 timing probe measures 172610 guest ticks/frame without glyphs and 458846 with 100 glyphs, versus 734791 for full coverage. This adds CPU work and is not a whole-game performance result. `ff9ds_text_coverage` is a fresh ROM/ELF with the exact PC briefing input schedule; game appearance and whole-scene timing validation remain pending. Prior saved states must not be loaded into this executable.

The fresh PC build with font-region notifications and the reordered raster loop completes the Princess Garnet route to 60000 frames and exits normally (`pc_text_coverage_60000.log`, `.png`). The new DS image is fresh-booted, with the original PC input schedule and no manual advance inputs. Runtime ITCM use is 17656 bytes. GPU ordering and all three GX texture/cache fixtures pass.

The first fresh-run screenshots do **not** validate dialogue filtering: a runtime snapshot during the instruction text shows zero selected font rectangles. The initial page predicate covered the main.c UI page 1E but missed script_text.c’s 13-high dialogue page 1F (`func_800515EC`). The matcher now includes both proportional-font pages, with a regression test for page 1F at height 13. A fresh `ff9ds_text_both_pages` image replaces that validation attempt. The apparent initial readability improvement was subjective and must not be treated as evidence that the filter had run.

The broader page predicate does not resolve the live handoff: `ds_both_font_pages_first_text.ml8` still has zero selected rectangles and zero retained filter-mask bits. Replaying the old instruction-text OT through the production decoder finds 43 font sprites on page 1E, so the missing-page hypothesis alone was incorrect. A targeted fresh probe records decoder font candidates, matcher/clip acceptance and DS glyph-call/presentation counters. The candidate is not yet visually verified or accepted as a readability fix.

The targeted first-dialogue probe (`ds_font_handoff_first_dialogue_probe.json`) confirms 7823 13-high candidates, 9799 total font matches and 8818 accepted glyph calls, with 18 glyphs at the last presentation. Thus the decoder/hook did run; the initially inspected zero live count was from the next in-progress draw. The empty retained mask points to buffer timing: text drawn into the next PSX framebuffer replaces the metadata for the currently displayed buffer before `PutDispEnv` switches it. The candidate now keeps the previous draw’s rectangles alongside current ones and chooses coverage by VRAM/display coordinates. A host fixture checks that next-buffer glyphs do not replace the displayed buffer’s thin-stroke coverage. Fresh `ff9ds_font_buffer_history` runtime validation remains pending. The added page 1F coverage remains correct for script_text.c, but missing that page was not sufficient to explain the old runtime evidence.

The fresh buffer-history run now confirms active coverage: `ds_font_history_first_dialogue_probe.json` reports previous_count 18 and 1144 retained filter-mask cells at game frame 4869 / guest VBlank 35461. The old probe had zero masked cells. `ds_font_history_first_dialogue.png` and `dark_dialogue_font_history_comparison.png` compare the actual filtered DS dialogue with the former nearest-font DS capture and DuckStation dark-room reference at DS size. The screenshot comparison uses window crops/resizing, not a pixel-exact PSX framebuffer extraction. Runtime animation and battle-performance checks remain pending.

The working PSX reference is saved in DuckStation game slot 1 at Vivi’s “Are you awright?” scene (`SLUS-01251_1.sav`, copied to ignored `build/validation/duck_vivi_arrival_reference.sav`; save confirmation in `duck_vivi_reference_saved.png`). DuckStation is closed while fresh DS validation continues, so only one emulator remains running. Relaunch the same DuckStation AppImage/disc and load game slot 1 for later Alexandria comparisons.

### Horizontal triangle edge coverage

The native first-dialogue VRAM snapshot contains 272 non-black pixels on display row 0, with rows 1–3 black; the DuckStation dark-room screenshot does not show that stale line. The rasterizer’s horizontal top-left test used B<0, excluding a top edge directed left-to-right in screen coordinates. Changing the horizontal case to B>0 covers the top row. A fixture compares a screen-aligned quad with a rectangle, including all four blend modes and opaque output; the 4096-primitive ASan/UBSan differential passes. Fresh PC at frame 4500 (`pc_top_edge_dark_4500.png`) now has zero non-black pixels on the top row. Fresh DS validation of this change is pending; the currently running buffer-history ROM predates it.

`tools/xkey.py` now sends the window manager an EWMH activation request before raw focus. This restores Qt savestate shortcuts after closing another emulator; the previous failed slot-8 save was retried successfully, with frame 22044 / guest VBlank 200996 recorded in `ds_font_history_name_route_resume.ml8`.

For manual candle animation capture, the matched buffer-history state at frame 4869 was copied with only `n_presses` set to zero (`ds_font_history_candle_manual.ml8` plus JSON). The same ROM was restored, dialogue advanced, Zidane walked to the candle and the actual “Light the candle / Cancel” prompt was captured (`ds_font_history_candle_question.png`). The untouched automated route is preserved at frame 22044 in `ds_font_history_name_route_resume.ml8`. This running ROM still predates the horizontal-edge fix; `ff9ds_top_edges_text.nds/.elf` is built separately for fresh validation.

### Text shimmer / visible intermediate bitmap (user report)

The user reports frame-to-frame movement on the candle prompt and naming screen, rather than the first dark-room dialogue. Consecutive naming frames from `ds_font_history_candle_light_reveal.mp4` show alternate crisp and softened glyph shapes (`name_consecutive_frames.png`). The scaler writes a nearest image and then filtered font pixels into the single bitmap being scanned out; its measured work outlasts VBlank. The candidate now uses two 256x256 Bmp16 buffers in VRAM A/B, renders into the hidden buffer, waits for VBlank and changes BG map base after both passes finish. Output stride is now 256. Software `plat_wait_frame` lets display_present perform that wait after rendering; GX retains its prior wait. Coverage/wrapping/border tests pass with the new stride. `ff9ds_double_bitmap` includes the top-edge fix and buffer-history font selection, but fresh DS shimmer/performance validation is pending. The fast text-only fresh-boot input schedule is saved separately and ends at frame 5200; it does not claim opening-to-battle coverage.

The fresh DS top-edge build now also has zero non-black pixels on display row 0 (`ds_top_edge_fresh_progress.ml8`, frame 11287, `ds_top_edge_probe.json`), confirming removal of the stale line on DS as well as PC. The saved source state is matched to `ff9ds_top_edges_text.elf`. The double-bitmap text probe is now fresh-booted for candle/name consecutive-frame validation; only melonDS is running. The user enlarged its window to 1303x1203, so earlier fixed screenshot/video crop coordinates must not be reused.

The older top-edge emulator failed to close via Alt+F4; launching the double-buffer probe without verifying exit briefly left two melonDS instances. After the user pointed it out, SIGTERM did not terminate PID 2204242; SIGKILL closed that specifically identified older ROM process. Verification shows only PID 2214624 with `ff9ds_double_bitmap_text_probe.nds` remains, window 0x6a00007 (716x875). Subsequent ROM switches must verify the old process is terminal before launching another instance. Fresh double-buffer probe reaches frame 4247 / guest VBlank 30267 without a crash; text stability still needs consecutive-frame comparison.

Fresh double-buffer naming-screen verification: `ds_double_bitmap_name_consecutive.mp4` records a static alphabet grid. Comparing 330 consecutive frames over 11 seconds in native-screen ROI (100,84,136,32), with a >24 RGB-change threshold to exclude minor lossy-codec noise, the old capture has 38 transitions with >10 changed pixels (max 369); the double-buffer capture has zero changed pixels above threshold. `font_jitter_before_after.json`, `name_consecutive_frames.png`, `name_double_buffer_consecutive_frames.png` preserve measurements and frame grids. This confirms removal of the reported naming-grid shimmer in that interval; candle-prompt and battle/performance validation still need completion. Default software and explicit GX builds pass; build/ff9ds.nds is restored to the manual software configuration (no scripted inputs).

Candle-prompt verification is also positive: `ds_double_bitmap_candle_prompt_consecutive.mp4` contains the actual “Light the candle / Cancel” prompt after manual replay from the matching frame-4247 state with only scripted-pad count disabled. Its text ROI (112,52,74,20) has zero >24 RGB pixel changes across 330 consecutive frames / 11 seconds (`candle_prompt_jitter_double_buffer.json`, `candle_prompt_double_buffer_frames.png`). The earlier 60-second file named `ds_double_bitmap_candle_name_stability.mp4` is entirely the naming screen; its contact sheet confirms it is not candle-action evidence. This fixes the reported shimmer in both tested static UI regions, not a claim that all graphics/performance issues are solved.

`ds_double_bitmap_candle_reveal.mp4` and its contact sheet now capture the real new-build candle sequence, from the selected Light prompt through hand motion, room reveal, fade and name screen. The actual sidebar Confirm action is selected in `ds_double_bitmap_name_confirm_selected.png`; the following manual dialogue route proceeds toward the first battle. These captures use the fresh double-buffer/top-edge ROM and therefore supersede pre-fix animation footage for this display configuration.

### Idle battle crash after Marcus KO

The new-build idle battle interval is invalid as an FPS measurement: both snapshots have game frame 29289 while guest VBlanks advance 247032 to 257055. The screen freezes on “Get some!” with Marcus knocked out. ARM9 is at PC 0 / abort mode, with no populated libnds exceptionRegisters. The first emulator abort logs point into ITCM sg_rect at 0100007C, followed by stack-related aborts and prepare_palette at 0209E42C. Comparing serialized physical ITCM with the matching ELF confirms code corruption at bytes 4,12–15,18–19 near sg_rect’s entry; IRQ-table/list changes later in ITCM are expected mutable data. Root cause remains unresolved. Do not attribute this to the display change without a reproducer or count this route as passing.

The bad PC generated a runaway 59-GiB emulator log and filled the host filesystem. The emulator was stopped and killed, the log’s first 256 KiB and last 16 KiB preserved, and repeated output removed; `melonds_double_bitmap_text_probe.truncated.json` records its original size. Free space is restored (~59 GiB). Future emulator runs use `tools/bounded_log.py`, retaining startup/latest output within ~1 MiB. The PC smoke harness now supports `--idle-after FRAME`; an enemy-attack/KO replay to 45000 frames with inputs ending at 26000 is underway. No emulator is currently running.

The PC idle replay completed 45000 frames without a fatal error (`pc_idle_ko_45000.log`), with party KO occurring, so it does not reproduce the native DS failure. A DS-only diagnostic build (`ITCMGUARD=1`) protects the first 16 KiB of linked ITCM with MPU region 7; DTCM retains region 6 access. Region 2 also makes the low 4-KiB ITCM alias read-only, to catch near-null packet writes. The diagnostic remains opt-in. `defaultExceptionHandler()` is now installed before game startup. The first guard build reached the dark-room/candle route normally; the alias-guard ROM is being booted fresh to trace the battle corruption. Each ROM switch verifies the previous melonDS process has exited. No old executable savestate is loaded into a new build.

The fresh alias-guard replay completed candle/name and reached battle. Its natural-run states at frames 11040 and 12677 show normal ARM9 execution, zero changes in the first 16 KiB of ITCM, and no captured exception. The interval measures 9.526 frames per emulated second at 60 guest VBlanks/sec (`itcm_alias_guard_idle_timing.json`); this remains a performance issue and is not a hardware wall-clock measurement. Two short idle battle clips and their contact sheet show enemy motion and Cinna's KO; the existing PSX combat recording supplies a visual reference. This is not a synchronized per-pixel or animation-timing match.

`ds_itcm_alias_guard_two_ko_natural.ml8` preserves the natural run at frame 24317 with Zidane and Cinna KO. A separate diagnostic copy changes only Battle.units[2].hp from 70 to 1 (`ds_itcm_alias_guard_marcus_one_hp.ml8.json`), to exercise Marcus KO sooner; it does not change executable code or set KO flags. Results from that copy must not be reported as an unmodified route pass. `tools/inspect_ds_state.py` reports saved CPU registers, exception registers, and ITCM differences against the matching ELF; it is a diagnostic tool, not a completion gate.

That controlled replay produces a captured exception after Marcus KO at frame 25469 (`ds_itcm_alias_guard_marcus_ko_probe.ml8`, `ds_itcm_alias_guard_after_marcus_ko.png`). The first 16 KiB of ITCM remains identical to the matching ELF. The stock handler itself then reports an undefined-instruction exception in getExceptionAddress, replacing its shared register snapshot; its displayed PC must not be attributed to the original game fault. Guard builds now install a minimal handler which saves the first 16 registers plus BIOS exception metadata into `port_itcm_first_fault` before printing anything, and avoids instruction decoding. `ff9ds_itcm_first_fault` is built and being replayed from fresh boot; the older crashed emulator was closed and its termination verified. The original low-pointer writer is still unresolved.


### Nullable texture-animation controls and systematic audit

The original DS idle crash is traced to `func_800BD5A0`, not the rasterizer: Marcus and Blank have `BtlUnit9C.unk50 == NULL`. Original disc MIPS at 800BD5E0/600/614/628 unconditionally stores at offsets 0x2c, 4, 0xc and 0x12. The writes at 4, 0xc and 0x12 match the damaged ITCM bytes exactly. The guarded native replay logs the original abort at ARM 02042088; a secondary BIOS prefetch abort explains why the ordinary exception display lost the game PC. Original disassembly is retained in `psx_marcus_ko_texanim_mips.txt`.

Counted patches now map dereferences of optional texture-animation data into the PSX low-RAM shadow, including KO, revival/status changes, battle scripts and summon helpers. Loader assignments and NULL tests retain their original meaning. The AST inventory finds 36 accesses across ten routines in this family, including a dereference embedded in assignment. `PSX_NZ` now evaluates its operand once, so a helper call is not repeated when non-null. `test-low-ram` checks the original KO store offsets, preservation of other bytes, unchanged live-pointer access and single evaluation under ASan/UBSan.

The PC controlled replay lowers Marcus and Blank to one HP at frame 26000, sees both absent-animation KO calls, and completes 65000 frames in the results screen without a fault (`pc_nullable_texanim_ko.log`). The first corrected DS ROM is freshly booted through candle/name/battle; its diagnostic copy lowers only those two HP values. At frame 23274 both are KO and battle keeps moving: the saved CPU state is normal, the first-fault marker is zero, and the first 16 KiB of ITCM matches its ELF (`ds_null_texanim_fix_after_marcus_ko.ml8`, `null_texanim_fix_after_ko_inspection.json`). The ROM includes the initial KO/status mappings; subsequently identified revival/summon/helper aliases are compiled in the normal build but still need fresh native validation. This is a controlled regression pass, not a complete game-route or performance pass.

`tools/audit_low_ram.py` uses Clang ASTs to inventory zero assignments/nullable returns and their propagation into member, indexed, cast and pointer-arithmetic dereferences. Reports stay in build/. The matched-source scan covers 24 files and 3859 bodies; 1229 routines contain conservative candidates in the initial report. One recovered argument-signature diagnostic in ovl_0c remains explicit in coverage. These counts do not mean that every candidate is a reachable null dereference: guarded and initialization-dependent accesses need control-flow and original-MIPS review. Indirect calls, unresolved integer/pointer flows, assembly/stubs, and header-only functions are not comprehensively covered. The native MPU guard remains an opt-in backstop; the full port goal remains open.


The audit also scanned 27 equivalent/draft C files (114 parsed bodies, eight routines with candidates). The known E4A3C stack-home ABI draft has a conflicting header signature and remains an explicit recovered frontend error; the port replaces that routine with native code. These reports therefore cover source inventory rather than proving complete executable coverage. The corrected controlled DS KO route advances into mode 8 after defeat with Masked Man's “Use a Potion to restore HP!” dialogue at frame 46965, first-fault marker zero and unchanged instruction region (`ds_null_texanim_fix_two_ko_followup.ml8`). This extends the passing regression beyond the immediate KO animation.


The controlled DS defeat route returns to the field, showing Baku's “You're lookin' a lot better!” dialogue (`ds_null_texanim_fix_after_defeat_dialogue.png`, `ds_null_texanim_fix_defeat_exit.mp4`, matched field-return state). The latest normal manual software ROM/ELF is preserved as `ff9ds_nullable_animation_family`; the full family and single-evaluation helper also pass the repeated 65000-frame PC KO regression (`pc_nullable_animation_family_ko.log`). Fresh DS validation of that expanded family, other audit candidates, graphics fidelity and performance remain outstanding.
### Additional disc-three fixtures and renderer priority (2026-10-13)

Metadata-only direct battle fixtures now cover Valia Pira (525), Taharka (871),
Garland (890), and the Kuja encounter bundle (891). The latter does not establish
the canonical story-dependent Trance Kuja formation. The CF5F8 vector helper
preserves the original unusual SQR register inputs and low-RAM reads; 1,200
original-CPU angle cases pass with the shared GTE/atan model. Seven-word custom
pool callbacks now enter the MIPS interpreter, and the offscreen renderer's fifth
argument is decoded as an OT pointer while scalar arguments remain unchanged.
The 360-case effect VM suite and both DS/PC builds pass.

The six-fixture host batch completes Ark and Meltigemini at 65,000 frames. The
four added fixtures all stop at the remaining DD27C implementation, rather than
the earlier raw callback or offscreen OT pointer crashes. Native validation of
these four routes remains pending; these are not completed boss tests.

The optimized, uninstrumented Ark battle interval measures 8.064 presentation
frames per emulated second (5,420 frames over 40,326 guest VBlanks). The controlled
128-frame profiling window measures 70.499 ms in GPU emulation alone, versus
16.667 ms for an entire 60 Hz frame. Full-quality software rendering is therefore
not a credible general 60 fps target on this hardware. Hardware renderer recovery
is the priority, retaining software as the image reference and fallback. Known GX
texture/geometry drops and blending artifacts must be resolved and checked on DS;
hardware acceleration by itself does not establish a 60 fps result.

The latest Ark native run reaches Game Over with no captured fault and no changes
in the guarded first 16 KiB of ITCM. Its static Game Over source and active bitmap
match the earlier validated output. The file named
`ds_ark_performance_live_late_combat.mp4` actually shows static Game Over and must
not be used as combat animation evidence. The reviewed Meltigemini effects clip
does show changing effects, but synchronized PSX morph-phase validation remains
pending.



### Native GX allocator recovery (2026-10-14)

The isolated `GXSHADOW=1` option enables polygon shadow replay without enabling
indexed generated textures, rectangle merging, or atlas planning. Four GX
fixtures cover that configuration independently. It did not fix native image
quality by itself: the actual candle approach/reveal clip
`ds_gx_shadow_isolated_candle_action.mp4` shows missing room tiles and large
rectangular light boundaries. The original PSX reference
`psx_geometry_shadow_light_action.mp4` shows the rounded light pool and reveal.
These clips show the corresponding actions, without synchronized positions/timing.

Native failure counters identify texture upload failures, with no heap, cache,
texture-name, or palette failures in the tested route. At the first failure,
submission 2970, a 16x16 four-bit texture (128 bytes) fails with only 154,624 bytes
in 453 pinned textures. Inspection of libnds' actual allocator from the matching
savestate shows `firstEmpty == NULL` despite 378,368 bytes free, including a
345,856-byte contiguous hole. The allocator loses its empty-list head when the
heap is completely filled and the first texture is then freed. The host bitmap
allocator recovers normally, explaining the earlier disagreement.

The project now compiles the matching libnds v1.24.0 videoGL implementation with
the single empty-list repair; provenance and license are in `third_party/libnds`.
The installed SDK is untouched. `test-libnds-allocator` extracts the actual
allocator functions and checks full-heap eviction, refill, coalescing and 20,000
allocation operations under ASan/UBSan. Removing the fix reproduces the failed
empty-list invariant. Host mocks now use the SDK texture-name limit and clear
texture names on deletion, matching the native API.

A fresh DS build with the allocator repair, `GXSHADOW=1` and `ITCMGUARD=1` reaches
frame 6066/naming with zero dropped draws or terminal upload failures. The reviewed
`ds_gx_allocator_fixed_candle_action.mp4` and contact sheet show the round light
pool restored and the room reveal, replacing the earlier missing tiles. The
matching state has no captured fault and no changed bytes in its protected code.
This is an opening regression check; later GX scenes, blending and text quality,
and acceptable performance remain unproven. The software path remains default.

The guard's earlier GX startup failure was a diagnostic layout error: the smaller
GX executable puts writable libnds IRQ/thread state inside the fixed first 16 KiB
of ITCM. The guard now selects the largest power-of-two prefix (up to 16 KiB)
before `irq_nesting_level`; this GX build protects 8 KiB plus the low-address alias.
State inspection reports the actual protected range separately from the first
16 KiB. The native guarded build boots and completes the tested candle route.
Both normal DS/PC builds and all four GX fixtures pass. Unused melonDS instances
are closed; captures, states and ROMs remain under ignored `build/`.


### GX Ark resource and timing check (2026-10-14, still failing)

The repaired allocator does not remove Ark's genuine working-set limit. In the
fresh `ff9ds_gx_ark_profile_frame1663` run (`GXSHADOW=1`), the first terminal
upload failure is submission 50: a 64x64 eight-bit texture needs 4096 bytes while
158 pinned entries own 523,008 bytes. The frame-1663 state records 170,216 dropped
primitives cumulatively; its visible image is largely absent. Its matched
128-frame profile ending at frame 1633 takes 241.5861 ms per presentation interval,
with 206.3701 ms in GP0 processing and 52.4559 ms in polygon shadow replay. This is
an incomplete render and cannot support a speed claim against the correct
116.6142 ms software reference. Both share the same 680.945 GP0 calls/frame.

`GXSHARE=1` alone also fails natively: its corresponding interval is 257.3871 ms
and its captured image remains incomplete. The host allocator likewise records
real capacity failures for this direct-entry Ark route. Adding the existing atlas
planner still drops textures. A temporary whole-indexed-page experiment worsens
the host result (325,410 cumulative texture failures over 2000 frames), so that
code and flag were removed rather than retained as another mode.

A native shared-cache snapshot contains 316 pixel images occupying 522,752 bytes;
294 are eight-bit images occupying 500,224 bytes. Many regions overlap within and
across PSX texture pages. Only 35 of those eight-bit images use at most sixteen
indices, so lossless four-bit packing alone would save about 56 KiB in that
snapshot. Coordinated reuse of overlapping source regions remains necessary.
The diagnostic byte accounting now counts shared images once and handles direct
fallback after indexed palette upload failure.

The battle timing and images are retained under `build/validation`, including
`ds_gx_ark_profile_entrance.mp4`. They establish a remaining failure, not successful
PSX parity or acceptable performance. The default software ROM is restored and
all native emulators are closed. GX remains opt-in; full-game crashes, effects,
text quality and performance work remain open.


### GX backbuffer lifetime and logical texture names (2026-10-14)

The nearly blank Ark capture has a presentation bug in addition to texture
pressure. A host trace shows scene submissions around frame 1611 with draw origin
320/display origin 0, followed at frame 1614 by a 45-polygon HUD update with both
origins 320. GX previously swapped on every emulated VBlank that drew anything,
so the HUD-only list replaced the scene. It now retains backbuffer geometry until
that buffer is displayed. The field can begin drawing its next buffer before the
next VBlank: the completed visible target is also submitted on that target change.
This prevents accumulating two field scenes into one geometry list. Composite
rectangles change target before allocating, keeping their shadow and texture pins
in the correct frame. The shared GPU layer exposes the display RECT explicitly.

Tests cover scene/HUD updates across VBlanks, repeated waits, clear-only buffers,
field target changes before VBlank, queued rectangle origins, and the first
composite on a new target. Indexed regions now use byte-aligned four-bit origins
and exact eight-bit origins, reducing unused padding. Pixel checks cover both
formats with non-grid origins and texture windows. In the 2000-frame Ark host
probe, tight origins reduce cumulative texture failures from 194,206 to 158,057;
combined presentation changes reduce that to 136,536. These are still failing
resource probes, not complete rendering or performance passes.

Native opening validation caught two regressions during this work: deferring only
at VBlank mixed field buffers and exhausted vertices, and indexed image/palette
sharing exhausted the SDK's 2048 CPU texture names. Those failed captures are
`ds_gx_retained_opening_candle.mp4` and its contact sheet. They must not be used as
approval evidence. Texture-name allocation now evicts eligible old entries and
retries without freeing pinned images. The SDK wrapper permits 8192 logical names
in its existing dynamic arrays; the GPU texture/palette VRAM budget remains
512 KiB/64 KiB. The host model matches this project-specific capacity. Native
name high-water reaches 3289 in the successful replay, so the larger allowance is
actually exercised.

The final `ff9ds_gx_target_switch_final` replay uses `GXSHADOW=1 GXSHARE=1` and the
ITCM guard. It reaches naming at frame 6898 with zero dropped draws or recorded
resource failures, zero first-fault marker and zero protected-code changes. The
reviewed `ds_gx_target_switch_final_candle.mp4` shows the prompt, rounded light,
room reveal and naming, compared with the existing original PSX candle reference.
Actor positions and wall-clock timing are not synchronized. GX text/blend quality
and later effects remain open. The intermediate Ark retained-scene clip restores
world/party visibility at frame 1663, but still loses textures and HUD text. Its
198.6229 ms instrumented interval is not a final-build performance result.

A separate prototype sharing original face bounds across clipped triangles did
not improve the host pressure result (139,017 failures versus 136,536 over 2000
frames) and was removed. Cache analysis shows substantial overlapping source data:
500,224 logical eight-bit bytes cover 257,280 physical source bytes. Only about
35 KiB of those images are already contained byte-for-byte in a larger resident
region. More coordinated reuse is still needed for Ark; software remains default.
Both default targets and renderer/OT/allocator regressions pass. Native emulators
are closed after captures; no disc data, captures or ROMs are committed.


### Lossless GX packing of sparse eight-bit regions (2026-10-14)

`GXSHARE=1` now packs an eight-bit region into a four-bit GPU image when its
converted indices, including a reserved transparent zero when needed, fit sixteen
entries. This changes index representation without quantizing colours. Opaque
black stays opaque. Regions that do not fit retain their eight-bit representation.
Logical source bytes and all 256 logical CLUT colours remain available for dirty
source/parent validation; compact GPU palettes are stored separately. Immutable
shared images can use different compact palettes, and old submitted materials
retain their original pixels and palette after source or CLUT changes.

All four GX fixtures pass under ASan/UBSan. Added checks cover every pixel in
256 deterministic regions with texture windows, transparent/opaque black, high
indices, CLUT mutation, immutable submitted data, shared images across CLUTs and
fallback when sixteen opaque indices need an additional transparent slot. The
real-game 2000-frame Ark host probe records 97,688 cumulative failed texture
requests versus 136,536 at the committed backbuffer/name fix, with no geometry
budget or palette failures. This is a reduction of about 28%, not a rendering
pass. The log is `gx_compact_no_views_ark_verified_pressure.log`.

Two larger-image approaches were tested and removed: arbitrary resident subregion
views did not improve the baseline; on-demand 64-word by 256-row columns increased
failures to 157,873. The earlier proactive physical atlas trial also increased
failures. None of those experimental modes or full-cache view scans remain in the
renderer. Their ignored diagnostic diffs/logs are retained under `build/validation`.

The native `ff9ds_gx_compact_ark_frame1663` replay reaches its capture point. The
reviewed `ds_gx_compact_ark_entrance.mp4`, contact sheet and final screenshot show
Ark's entrance/camera motion and more of the HUD, but still missing floor and HUD
textures. They were compared with the original PSX
`psx_direct_ark_entrance_combat.mp4`; inputs/animation phases are not synchronized.
The frame-1087 savestate records no fault and no changed protected ITCM bytes.
After emulator keyboard focus stopped responding, a separate stopped-process RAM
read at frame 1663 verified matching RAM aliases and recorded 81,963 cumulative
texture failures, no geometry-budget failure, 3,244 compact material entries and
no ITCM fault marker. This RAM-only capture is not a loadable savestate; its
report is `ds_gx_compact_ark_frame1663_live_report.json`.

The matched instrumented 128-frame window ending at 1633 takes 180.8631 ms per
presentation interval, including 144.3383 ms of GP0 work at the same 680.945 calls
per frame. It still drops geometry and remains slower than the correct software
reference. It establishes neither acceptable performance nor PSX image parity.
Software remains the default. Native emulator instances are closed after capture
and the normal SD configuration is restored. Battle texture pressure, remaining
blend/font differences, missing routines and full-game validation remain open.


### Eight-texel GX allocations (2026-10-14)

Indexed face/glyph regions and generated subtractive composites now use the
hardware's 8x8 minimum instead of 16x16. Logical texture keys encode dimensions
in eight-texel units with six bits per dimension, keeping 8x8, 8x16, 16x8 and
256x256 allocations distinct. Parent/source validation and direct-image content
lookups decode the updated dimensions. The optional batch planner's region keys
also distinguish those sizes. Direct texture alignment remains unchanged.

All four GX fixtures pass under ASan/UBSan, including added byte/pixel checks for
small four/eight-bit regions, tall/wide key separation and a contained request in
a full page. In the same 2000-frame Ark host probe, texture failures fall from
97,688 to 55,011. Successful additional draws now expose 380 cumulative geometry
budget failures. Detailed host reasons show no cache, heap, name or palette
exhaustion: remaining texture failures are direct/shared GPU allocations.
This is still an incomplete rendering result.

The native `ff9ds_gx_eight_texel_ark_frame1663` replay and matching fresh state
record 44,297 cumulative texture failures and the same 380 budget failures, with
3,859 compact material entries, no first fault and no changed protected ITCM bytes.
The matched instrumented window ending at 1633 takes 155.0068 ms per interval,
including 118.6631 ms of GP0 work and 40.3619 ms of polygon shadow replay. Phase
costs overlap. This remains slower than the correct software reference and does
not establish acceptable performance. The reviewed entrance video/contact sheet
and screenshot still show missing floor/HUD textures against the original PSX
reference. Native artifacts use the `ds_gx_eight_texel_ark_` prefix.

This replay ran on a temporary private Xvfb display, which allowed reliable input
and fresh state capture without taking desktop keyboard focus. The emulator and
temporary X server are closed, and normal SD paths are restored after validation.
The software renderer remains default. Resource failures, blending/font quality,
remaining routines and full-game validation are still unresolved.


### GX offscreen generated textures (2026-10-14)

The complete-frame request trace exposed a second source of missing graphics:
GX skipped every primitive targeting a small offscreen DRAWENV. At frame 1663,
the 112x112 region starting at (656,400) is all zero in GX but contains 141 colours
in the correct software renderer. The GX wrappers now rasterise offscreen
polygons, lines and sprites into PSX VRAM with the existing software rasteriser.
They notify texture/deferred-source tracking before writes. Main-screen geometry
continues through GX; generated textures can be sampled by later draws.

Added integration checks compare all VRAM after 48 combinations of primitive
kind, texture depth, blending and mask bits. Further checks cover cache
invalidation with immutable submitted pixels, and a queued screen sprite that
must sample the old source before an offscreen producer changes it. All four GX
fixtures pass under ASan/UBSan. The complete 2000-frame host route still fails
capacity checks (59,104 texture failures, 380 vertex-budget failures); restoring
real source data costs more than caching blank images.

In the fresh native `ff9ds_gx_offscreen_ark_frame1663` replay, all 12,544 words of
that generated region now match the software frame exactly. The separate
(960,432,64,80) value cache remains blank in GX versus 343 colours in software;
its producer/readback still needs investigation. The state records 56,260
offscreen primitive calls, no fault and no protected ITCM changes. It also records
44,297 cumulative texture failures and 380 budget failures. The matched window
takes 162.5809 ms per interval (130.3112 ms in GP0), so neither performance nor
rendering parity is achieved. Reviewed native entrance video and screenshots
still show missing HUD/floor graphics against the original PSX reference.
Artifacts use `ds_gx_offscreen_ark_`; exact generated-texture comparison is in
`gx_offscreen_generated_texture_comparison.json`. The private emulator/display
are closed after capture and normal SD paths restored. Software stays default.


### GX backbuffer readback and sprite mask bits (2026-10-14)

An original-game software watchpoint identifies the remaining blank value cache:
`ContinueDraw` copies a 32x48 area from backbuffer (528,167) to texture memory
(960,432). GX drew the source into its destination shadow but GPU copies and
`StoreImage` read the stale CPU VRAM buffer. A weak readback hook now materialises
known shadow pixels before either operation. It flushes deferred draws first,
handles source wrapping and retains completed backbuffers in CPU VRAM before
starting another target. Unknown pixels are not invented.

Sprite shadows now use the existing PSX rasteriser through a rectangle target
helper, preserving blending, transparency, mask testing and the complete PSX
colour/mask word. Generated subtractive composites preserve their source mask
bit too. A re-entry guard prevents a texture-generation notification during
submission from recursively flushing the rectangle currently being submitted.
The independent raster builds rename the new shadow helper alongside their
existing entry points.

GX checks cover pending/current and completed backbuffers, mask testing, wrapped
reads and unknown pixels. GPU integration checks exercise the real StoreImage
and MoveImage entry points, source wrapping, full colour/mask words and
read-before-destination-write notification. All four GX fixtures and the ordering
checks pass under ASan/UBSan; 4096 mixed software raster primitives still match
the generic path byte for byte, and the separate DS raster binary builds.

The native nonwrapping Ark replay (`ff9ds_gx_readback_ark_frame1663`) restores 337
colours in the previously blank value cache. Of its 5120 words, 912 still differ
from the software reference; the live source rectangle differs at 47 pixels.
Skipped/approximated source geometry remains a known limitation, not a readback
parity pass. The other generated 112x112 region continues to match all 12,544
words exactly. The matching native state has no captured fault or protected ITCM
changes. Its instrumented window takes 170.2855 ms (134.9037 ms GP0 and 9.9549 ms
sprite shadow rasterisation); costs overlap and the render still drops graphics.
The final wrapping branch is additionally checked in the host fixtures.
The 2000-frame host route records 59,821 texture failures and 380 budget failures.

The reviewed native video and screenshot still show missing HUD/floor graphics
against the PSX reference. Native artifacts use `ds_gx_readback_ark_`; generated
texture comparison is in `gx_readback_generated_texture_comparison.json`. The
emulator/private display are closed and normal SD paths restored. Software stays
default; texture/vertex capacity, remaining blends/fonts, missing routines and
full-game correctness remain unresolved.


### PSX shadows survive GX resource failures (2026-10-14)

Readback still differed because shadow updates ran only after successful GX
allocation. GPU-clipped triangles also fed rounded intersection vertices back
into the CPU shadow. Polygon shadows now consume the original PSX primitive
before GX clipping, upload or vertex-budget checks. Sprite shadows likewise
update before a failed hardware draw, and CPU-only target updates survive
submission. The GPU failure counters still report the missing hardware draws.

The added test rejects every textured upload and compares a complete backbuffer
after 96 mixed clipped triangles/quads, with Gouraud shading, blending and mask
bits. Its CPU pixels match the software rasteriser byte for byte. All four GX
fixtures pass, along with the raster/ordering/allocator checks and default DS/PC
builds. Native frame 1663 now matches all 19,200 words in the three traced regions:
the offscreen effect texture, the cached value atlas and its backbuffer source.
The previous 912 atlas differences and 47 source differences are gone.
`gx_independent_shadow_generated_texture_comparison.json` records that comparison.

This repairs PSX readback semantics, not GPU capacity or performance. The native
state still records 42,353 texture failures and 432 budget failures. The reviewed
battle motion and screenshot restore the command-window border, with floor/HUD
pieces still absent against the original PSX reference. The matched instrumented
window takes 193.9218 ms per interval: 158.077 ms GP0, including 58.605 ms polygon
and 14.8519 ms rectangle shadow work. Phase costs overlap. Preserving all PSX
pixels costs more CPU time; full CPU shadow replay remains a performance issue.
The 2000-frame host route likewise still fails (57,559 textures, 432 budget).
Native artifacts use `ds_gx_independent_shadow_ark_`.

The same uninstrumented renderer reaches opening/naming frame 9613 with zero
dropped draws or resource failures, no captured fault and no protected ITCM
changes. The real candle transition is captured using the new
`capture_ds_at_frame.py` helper, which waits on the live frame counter and
checks the ROM/native code before injecting input. It produces a fresh prelight
checkpoint at frame 4807 and a focused action clip. The helper also rejects an
already-passed frame, a mismatched ELF and a short timeout without creating
output. Native candle/naming footage is compared with the existing original PSX
candle-action reference; positions and wall-clock timing are not synchronized.
GX font/blend differences remain visible. All generated artifacts stay ignored.
Post-action frame 6626 also has zero resource drops, faults and protected ITCM
changes. The focused 35-second clip is
`ds_gx_independent_shadow_candle_action_short.mp4`; its source recording includes
the choice, lighting and room reveal. The emulator/private display are closed
and normal SD paths restored after recording.
Software remains default; missing draws, acceptable performance, remaining
routines and full-game validation are unresolved.

GX shared mode now also packs direct-colour textures and generated composites
into exact 4-bit/8-bit palettes when they contain at most 256 colours. No colour
quantization is used. Transparent RGB is normalized while opaque black remains
distinct; larger images and palette-allocation failures retain RGBA storage.
Original CPU pixels remain available for cache validation and PSX readback.
A linear hash encoder replaces the composite encoder's quadratic colour search.

Five GX sanitizer fixtures now include the actual SHADOW+SHARE configuration
without EXPERIMENTAL. They check colour-count boundaries, transparency discovered
last, immutable shared pixels with distinct palettes, source changes, palette
failure fallback and 512 random encoder cases. Default DS/PC builds and the
raster, ordering and allocator checks pass.

The new `tools/verify_ds_gx_direct_textures.py` reads native texture/palette banks
from complete melonDS states, using matching ELF/DWARF layouts and SDK handles.
It compares resident direct/generated texels against their original CPU pixels
and all shared image uploads against physical texture RAM. This verifies storage,
not screen rendering or draw completeness. The prior RGBA candle control passes
167,296 texels. The packed candle checkpoint passes 168,384 texels across 281
materials (78 RGB16, 198 RGB256, 5 RGBA), with all 558 shared uploads identical.
The Ark checkpoint passes 1,792 direct texels and all 1,039 shared uploads.
The three generated-source regions also retain their exact software reference
values across all 19,200 words.

The 2000-frame Ark host route reduces texture failures from 57,559 to 54,803;
direct-texture failures fall from 10,835 to 50, but indexed allocation pressure
still dominates. Native frame 1663 records 42,138 texture failures and 451 budget
failures. Its matched profiling window is 194.3136 ms, so this is a storage
improvement, not a measured speed improvement. The battle still lacks floor/HUD
pieces. Native candle footage shows the choice, lighting and room reveal against
the PSX action reference; font/blend differences remain. Opening frame 9953 has
zero resource drops, captured faults and protected ITCM changes. Artifacts use
`gx_direct_palette_` / `ds_gx_direct_palette_` under ignored `build/validation`.
The emulator/private display are closed and software remains the default.

The completed-upload atlas experiment is rejected. It qualified small 64-word
asset uploads, excluded framebuffers, checked converted face pixels against an
immutable atlas and kept old images pinned. Across the same 2000-frame Ark route,
canonical widths of 512, 256, 128 and 64 PSX words cause 68,158, 61,727, 61,727
and 63,898 texture failures respectively, versus 54,803 without the experiment.
Large allocations become eligible after earlier scene textures already occupy
VRAM; smaller tiles still add unused texels and cross-boundary duplication.
Lower polygon-budget failures in these runs are not an improvement: more faces
fail texture allocation before reaching that budget. None of this atlas code
or its upload hook remains in the port. The patch/logs stay ignored under
`build/validation/gx_uploaded_asset_atlas_rejected.patch` and `gx_asset_*`.

To choose the next native game patch from actual costs, optional profiling now
separates texture material preparation, pixel/palette uploads, the C58E4 mesh
packet emitter and OT processing. Existing profile records remain compatible.
All five GX sanitizer fixtures, 4096 raster comparisons, OT/allocator checks,
160 packet comparisons and both default builds pass. The matched native Ark
window ending 1633 measures 191.8324 ms total, 156.7545 ms GP0, 68.4489 ms GX
material work, 1.0948 ms pixel uploads, 0.0154 ms palette uploads, 5.6587 ms mesh
packet generation and 11.8663 ms GTE. Polygon/rectangle shadow replay takes
58.6132/14.8608 ms. These phases overlap; OT's 158.6577 ms includes rendering.
This diagnostic is not a speedup claim or a complete timing of all game code.

The measurement supports replacing per-face material reconstruction with a
native asset-load layout and moving model projection/rendering to DS hardware.
Eliminating packet encoding alone cannot recover the much larger material and
CPU shadow costs. Native work must preserve PSX ordering, animated textures,
mask/blend semantics and readback-generated effects, with the existing path as
the reference. Full CPU shadow drawing should become demand-driven for actual
readbacks rather than duplicate every model draw.

The fresh frame-1663 state retains exactly the preceding 42,138 texture failures
and 451 budget failures, no captured fault and no protected ITCM changes. All
1,039 uploaded image byte arrays and 1,792 resident direct texels pass the native
bank verifier. The 45-second motion clip starts from a fresh frame-1007 state;
it and the final screenshot are reviewed against the original PSX Ark entrance
and HUD references. Floor/HUD-name gaps, blending and text artifacts remain.
Artifacts use `ds_gx_compatibility_profile_ark_`. The emulator/private display
are closed, normal SD paths restored, and software is still the default.

Battle texture animation (`o0b` C9040) repeatedly issues a 96x190 MoveImage
from (640,1) to the same coordinates while its animation offset is zero.
The copy now returns before materialising readback, copying rows or invalidating
texture generations when source/destination addresses are identical after VRAM
wrapping and force-mask is disabled. Empty rectangles also avoid those hooks.
Force-mask operations and actual moves keep the existing path. The ordering
fixture checks unchanged pixels, absent read/write notifications, wrapped
identity moves, empty rectangles and the force-mask/non-identity exceptions.
This removes unnecessary work; it does not resolve the remaining GX capacity,
HUD routing, blend or performance issues.

An opt-in `GXLAZY=1` renderer now retains two independent framebuffer histories
and replays only queried 8x8 tiles. A tile's command progress preserves ordering
and avoids applying a blend twice. Source writes rasterise each dependent draw's
whole destination footprint and retire it, leaving unrelated geometry deferred.
The bounded 4096-command queues have an exact full-replay fallback. Partial CPU
destination writes first preserve the outstanding framebuffer, while complete
replacement can discard its previous pixels. Texture windows, CLUTs, dirty
source generations, mask bits and failed GPU allocations retain PSX semantics.
After a partial CPU write, importing its pixels preserves the previous unknown
coverage mask; a line unsupported by the shadow compositor must not become a
known background merely because another part of the framebuffer was written.
The new `psx_softgpu_shadow_poly_region` API preserves the original fixed-point
interpolation anchor while limiting traversal: changing GP0 clipping alone
introduced rounding differences, caught by the full-buffer differential test.

The source-write investigation also identifies an actual HUD routing bug:
F4A00/ContinueDraw installs E3/E4/E5 commands for the other backbuffer without
updating the cached DRAWENV. The old hardware route classifies those draws as
offscreen. GXLAZY tracks the two full-size targets and uses the live clip to
select their parent, including narrow HUD DRAWENVs. This depends on its separate
CPU histories; the legacy eager path remains available for comparison.

Six GX sanitizer variants pass. Added coverage includes 360 mixed draws across
three framebuffer locations, repeated overlapping reads, source/CLUT mutations,
partial CPU writes, cache/source ordering and queue overflow. The raster suite
adds 256 shuffled-region comparisons with the generic full-primitive reference;
the existing 4096 mixed-primitive and 96 forced-upload-failure tests still pass.
`CAPTURE_READBACK=1` supplies an explicit full readback at a diagnostic halt,
after timing publication, so native deferred pixels can be compared completely.

The native Ark frame-1663 checkpoint matches both complete 320x218 software
backbuffers (139,520 words) and all three previously traced source/cache regions
word for word. Its 1066 uploaded images and 1792 resident direct texels also
match physical GPU banks. No captured fault, protected ITCM changes, heap/name
or palette failures occur. Its 128-interval window ending 1633 measures
162.5809 ms, including 19.8911 ms polygon and 10.8591 ms rectangle replay.
The preceding full-drain prototype took 226.0462 ms; its source writes incorrectly
forced unrelated pending geometry to render. There are no queue overflow drains
in this Ark checkpoint; 11,469 source-dependent draws have been retired.
These are instrumented diagnostics, not an uninstrumented FPS result.

The fresh eager control with the identity-copy fix measures 184.1278 ms in the
same window (58.035 ms polygons, 14.8543 ms rectangles). Its full readback also
matches both software backbuffers exactly. Deferred replay measures 162.5809 ms
while additionally routing HUD draws that the control skips on the GPU. This is
an improvement over that hardware path, not over the 116.6142 ms software
reference. The difference does not establish a finished renderer or 60 fps.

This is not full screen parity or adequate performance. Ark still records 43,958
texture failures and 34,524 vertex/polygon budget failures. The newly routed HUD
draws expose limits previously hidden by skipping them. Reviewed native motion
against the PSX entrance/HUD references still shows missing floor/name elements,
HUD clipping/flicker, font and blend differences. Native material preparation
and a batched/native HUD remain necessary. Native artifacts use
`ds_gx_lazy_retire_` / `gx_lazy_retire_`; default software output is unchanged.

The opening route also reaches the candle choice, lighting animation, room
reveal and naming screen in melonDS. The 60-second
`ds_gx_lazy_retire_candle_complete_action.mp4` replay starts at game frame 5101
from the saved prelight state. Its sampled reveal and naming frames were
compared with `psx_geometry_shadow_light_action_contact.png`; the GPU text edges
remain visibly rough. The candle/naming GPU-bank checks report zero differences
across 167,872 and 15,040 resident texels respectively, which establishes stored
texture correctness rather than screen parity. Captured opening states have no
fault, protected ITCM changes or GPU resource failures.

One continuous uninstrumented run stays on the naming screen from frame 12,726
at guest VBlank 56,333 to frame 19,980 at VBlank 70,841: exactly 30 game frames
per emulated second. This is a static naming-screen measurement, not a battle
or whole-game rate. A separate ten-second naming recording has no large motion
in the sampled static “Pick a name” text region; it does not establish that all
font instability has been fixed.

The final partial-write coverage fix was rerun with the matching
`ff9ds_gx_lazy_refresh_readback_ark` ROM/ELF. At frame 1663, both full
backbuffers and the three source regions still have zero differing words; all
1066 uploaded images and 1792 direct texels still match GPU banks. The guard
and fault checks remain clear. The same 128-frame window ending 1633 measures
161.7974 ms (19.8913 ms polygon replay, 10.8595 ms rectangles, 66.2501 ms material
preparation), with the same texture/budget failure counts. Both DS and PC default
builds pass after this fix. The diagnostic emulator was closed and the normal
SD configuration and default software build restored.

The follow-up host allocator snapshot at GX submission frame 400 (game frame
1698) has 1010 unique images occupying 515,936 bytes: 448,736 bytes referenced
this submission and 67,200 only by the previous one. The eight-bit page key
`0x700000e2` alone has 217 cropped images occupying 110,880 bytes, exceeding the
65,536 bytes of its full page. This identifies duplication, but does not prove
that a full-page allocation fits alongside the other pinned textures.

Fresh 1800-frame Ark host runs with GXSHADOW/GXSHARE/GXLAZY give:

| Planner | Texture failures | Geometry budget failures |
|---|---:|---:|
| Disabled | 48,653 | 36,842 |
| Existing GXPLAN | 54,701 | 27,995 |
| Trial conservative compact-crop byte cost | 69,857 | 21,670 |

The trial required an eight-bit atlas to justify twice its area, accounting
conservatively for four-bit packed face crops. Its increased texture failures
reject that heuristic; it was removed. The existing planner remains opt-in.
These allocator counters measure dropped requests, not rendered image quality
or DS timing. Probe artifacts are `gx_lazy_material_occupancy*`,
`gx_lazy_next_material_runtime.log`, `gx_lazy_plan_comparison*` and
`gx_lazy_plan_compact_cost*`. Native model material layout and HUD batching still
need implementation and screen validation.

### Effect mesh variant and systematic export argument audit (2026-10-14)

DD27C now shares the native eight-list mesh emitter with C58E4. It preserves the
extra signed OT bias, the variant's page/flag handling, untextured Gouraud E1
prefixes and semitransparency, and its different culled-triangle delay slot.
The expanded 352-case original-instruction differential checks both emitters,
all eight formats, complete packet/OT buffers, returned offsets, model output
pointers and GTE state. Output buffers begin with a nonzero sentinel to expose
unintended writes to culled packets. The shared GTE model does not independently
validate GTE hardware accuracy or rendered pixels.

The four previously blocked fixtures pass DD27C. Valia Pira, Taharka and Garland
then stop at missing D59D0. Kuja initially exposed a raw module scale-vector
pointer passed to E7ACC (export +0x2A4); its unit ID must remain scalar. After
fixing that argument, Kuja reaches missing D5568 instead of SIGSEGV.

`tools/audit_effect_exports.py` independently checks the generated 216-function
table against native DWARF signatures. This identified 68 further entries with
omitted declared data pointers, plus printf's fixed format pointer. Including
the Kuja fix, 70 exports gain 120 mapped argument positions. Callback arguments
remain raw code addresses, and scalar bit patterns are preserved. The bridge
tests additionally cover pointer arguments on the stack, including positions
9–11, alongside E6764's unmodified callback word at position 13. The 360-case
VM differential and bridge checks pass. Both PC and ARM audits report zero
missing bits and zero unresolved signatures. Six optimized scalar wrappers use
source-checked reviewed declarations; printf uses its stdio.h fixed parameter.
Removing E7ACC's pointer bit in a temporary copy makes the audit fail as expected.
This is not an audit of scalar-typed pointers, nested structures or varargs.

Both default builds pass. The six-fixture 65,000-frame host batch retains Ark
and Meltigemini passes and reports the four explicit missing emitters above
(`scenarios_dd27c_export_audit/report.json`). Garland's native software run
reaches frame 2875 with clear fault/ITCM checks and records Flare, the arena and
subsequent combat in `ds_dd27c_garland_entrance.mp4`. Recovered original PSX
clips `psx_garland_reference_recovered_motion.mp4` and
`psx_garland_flare_finish.mp4` show the corresponding arena and full Flare
sequence. The synthetic fixtures choose different targets/timing; these are
motion/layout comparisons, not pixel-aligned or timing-parity proof. A prior
4.9-second PSX recording interrupted by display-server shutdown is explicitly
named `psx_garland_reference_motion_interrupted.mp4` and is not a complete clip.

Native Kuja passes the prior pointer crash and reaches the missing D5568 stop
at frame 2539 (`ds_dd27c_kuja_stop.ml8`, screenshot and inspection JSON), with
zero first-fault markers or protected ITCM changes. Two short actual videos
cover the route. The stopped close-up exposes a suspicious vertical pattern
along the arena's top edge; matching original PSX validation is still needed.
Neither four-boss completion nor visual parity is established. The test
emulators were closed and the normal SD configuration/default build restored.

### Gouraud effect fan, line trail and tail-spawn callback (2026-10-14)

D59D0 now emits its three Gouraud triangles and final E1 packet, preserving the
first RTPT's MAC0 early return and depth, partial vertex-register updates,
DPCT color fade, bucket bounds and OT tag bits. D5568 projects the circular
position stream into Gouraud line segments, preserving endpoint adjustment,
per-segment DPCS fade, wraparound and preceding draw-mode packets. It decodes
the ring pointer explicitly at its native dereference boundary. The effect
primitive differential suite now covers 900 cases across rectangles, fans and
lines, including both raw guest and native ring pointers. Complete packet/OT
buffers and GTE state match the original instructions with the shared GTE model.
The fan's internal D597C helper executes as original instructions in the oracle.

Passing D5568 exposed a native jump into module code (0x801E8C84) from E2000's
`EffTailSpawn.unk3C` callback. That four-word callback now enters the effect VM,
preserving its two SVECTOR output buffers and return value. The counted source
patch changes only the portified copy. A bridge fixture exercises output writes
and frame/duration return behavior with both native and guest pointer words;
the existing 360-case VM suite still passes.

Taharka and Garland now reach the 65,000-frame host limit; Kuja does too after
the tail callback fix (`scenarios_d59d0`, `scenarios_fan_line_tail_final`). Their
idle-input routes eventually reach Game Over; these are not boss-victory tests.
Valia Pira advances to missing D50A0 and remains blocked. A premature host probe
launched before the updated binary linked is preserved as
`scenarios_fan_line_before_link` and is not validation of the callback fix.

The matching D59D0-only Garland DS run advances from frame 4815 to 6575 through
Wave without a fault or protected ITCM changes. Its 45-second actual recording
`ds_d59d0_garland_fan.mp4` includes the cast and subsequent combat. The original
PSX breakpoint reaches D59D0 from D2938, and `psx_garland_wave_action.mp4`
records the corresponding 12-second original sequence. The native Kuja build
with both emitters and the callback fix records Thundaga and subsequent combat
in `ds_fan_line_tail_kuja_action.mp4`. It progresses beyond the old frame-2539
stop and eventually reaches Game Over with clear fault/ITCM checks. The later
frame-502513 checkpoint is predominantly idle Game Over time and must not be
used as a combat-duration or FPS result. Its top-edge close-up pattern remains
an open comparison item. Native artifacts retain matching ROMs, ELFs and states.
Both default targets build and the declared export signature audit stays clear.

The original Kuja trail breakpoint reaches D5568 from D0FFC; the completed
`psx_kuja_thundaga_action.mp4` clip records the original lightning sequence and
subsequent Demi cast. `psx_kuja_trail_entry.png` shows a corresponding close-up
without the DS top-edge vertical pattern. Both DS source framebuffers in the
stopped frame-2539 state contain the pattern, so it precedes bitmap scaling.
The host frame-2539 image has a separate striped silhouette detail; these two
patterns have not been proven to share a cause. A trial polygon UV clamp
changes zero pixels in that host frame. A half-texel interpolation-bias trial
changes 28,686 pixels without removing the silhouette pattern. Both trials were
removed, and the regular PC binary rebuilt. Raw PSX/DS VRAM dumps are retained
for diagnosis but come from different action phases and do not establish
source-texture corruption. All reference/test emulators are now closed.

Review of D5568's new ring decoding also caught a port-side narrowing error:
PSX_ADDR masks every address into the 2 MB arena, including native heap pointers.
The emitter now uses the existing `port_effect_data` boundary, retaining native
pointers outside that arena while decoding raw module addresses. The 900-case
effect suite includes ring data at native 0x02400080 as well as native/guest
addresses within the arena; the original-instruction oracle retains its own
guest ring location. The strip probe discards unused emitter/VM sections, while
the effect primitive probe links the production decoder itself. This correction
does not change the already-captured route's in-arena ring addresses.


### PsyQ environment clears and Kuja texture corruption (2026-10-15)

The stopped Kuja state's sky texture at VRAM x896..1023, y28..55 contains
3,584 copies of 0x0082. Background-OT replay reproduces the visible black/blue
bars on the host, establishing that the resident texture is already damaged.
A host breakpoint traces the overwrite to PutDrawEnv after E6E1C renders an
112x112 offscreen effect. Its saved environment produces a negative width;
the port's unconditional absolute fill masks that into a large positive span.
Original MIPS confirms the unusual saved-environment copy, so game sources
remain unchanged.

Original SetDrawEnv (80013EF0) always emits a clipped TILE for its background.
PutDrawEnv calls 800140E8, which uses an absolute fill only when both the
original X and clamped width are multiples of 64. Both clamp signed dimensions
and drawing-area corners to the GPU limits; TILE coordinates subtract the
DRAWENV offset with 16-bit wrapping. The port now follows these two builders.
A 1,024-case original-instruction comparison executes their real helper code
and checks all 64 packet bytes, including untouched bytes. A production
GP0/software-raster regression checks aligned/unaligned clears and preservation
of all VRAM for a negative-width saved environment. This also exposed signed
left-shift UB in GP0's 11-bit coordinate extension; equivalent masked arithmetic
now passes sanitizers.

The host frame-2539 silhouette stripes disappear. Native DS Kuja advances from
checkpoint 2435 to 3051 through Thundaga and subsequent Demi without protected
ITCM changes or a recorded ITCM fault. The formerly overwritten sky region now
matches all 3,584 original PSX texture words exactly (zero differences), and
its blue/black bars disappear. Artifacts: ds_drawenv_fix_kuja_action.mp4 (30s
actual animation), matching archived ROM/ELF, before/after states,
ds_drawenv_fix_kuja_texture_comparison.json, and pc_kuja_drawenv_fix_2539.png.
The clip was inspected alongside psx_kuja_thundaga_action.mp4; their targets,
RNG and action phases differ, so this establishes the texture fix and effect
progress, not whole-animation pixel or timing parity. The capture precedes
the equivalent coordinate-UB cleanup. Both default targets, test-gpu,
test-raster (4,096 primitives), test-display and five 65,000-frame host battle
fixtures pass. The replay diagnostic prototype is retained under ignored
build/validation; it is not a supported test interface yet.


### Valia Pira Holy ribbon emitter (2026-10-15)

The direct Valia Pira fixture first stops at missing D50A0, reached at host
frame 5673 from D0E14/D1494. The native implementation projects joined GT4
ribbons in 3D: OP/SQR and the reciprocal-root table normalize the segment/view
cross product, GPF applies signed width, and DPCS fades the two edge colours.
It restores the three saved diagonal rotation words before RTPT, preserves
cached joined edges and halfword-only UV stores, and retains the original
existing-MAC0 cull without adding NCLIP. Nested ring words use the production
mixed guest/native data boundary, including native heap addresses beyond 2 MB.

The expanded ribbon test executes original MIPS with the production GTE and
compares context, all 1,024 packet bytes, the full OT and raw GTE registers in
700 cases: 300 new 3D ribbon cases plus the existing 400 ribbon/particle cases.
New cases cover ring wrap, raw/native pointers, external native heap data,
coincident segments, signed widths, UV/fade wrapping, culling, and the original
first iteration even for nonpositive counts. This is a CPU/packet comparison
with a shared GTE, rather than an independent PSX hardware GTE validation.

The native DS checkpoint advances from 5119 to 6463 with zero recorded ITCM
faults or protected changes. ds_d50a0_valia_action.mp4 records 30 seconds of
actual Holy animation, including blue ribbons, particles and the flash. The
original PSX breakpoint reaches D50A0 at VBlank 7169 from D0E14; the 12-second
psx_d50a0_valia_holy.mp4 records the corresponding original sequence. Both were
inspected in motion/contact sheets. Targets, HP and RNG differ, so these clips
establish effect progress and visual comparison, not frame-aligned parity.
Matching ROM/ELF, states, logs and captures remain under build/validation.

Valia Pira subsequently exposes a separate null-coordinate read at frame 8043
and model-split pointer handling at frame 8219. Those remain separate work;
the new emitter does not establish full encounter completion. The five prior
host battle fixtures still pass 65,000 frames, and both targets build.
