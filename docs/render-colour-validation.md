# PSX colour conversion and dithering

The software renderer previously discarded the GPU's dithering flag. It now
applies the signed 4×4 matrix at VRAM coordinates before reducing eligible
eight-bit colour channels to five bits. Textured modulation retains its low
three bits until this step. Shaded polygons, modulated textured polygons and
lines follow the draw-mode flag; sprites, raw textures and flat untextured
polygons remain undithered. Dithered flat polygons bypass the single-colour
palette cache because their result depends on pixel position.

The matrix and primitive eligibility follow the
[PSX GPU specification](https://psx-spx.consoledev.net/dev/ps1/gpu/misc/#24bit-rgb-to-15bit-rgb-dithering-enabled-in-texture-page-attribute).
Quantization occurs before five-bit semi-transparency blending; controlled
draws also agree with the
[DuckStation software rasterizer](https://github.com/stenzek/duckstation/blob/master/src/core/gpu_sw_rasterizer.inl).

## Controlled reference draws

`tools/test_raster_psx.py` uploads synthetic textures/palettes and executes GP0
packets through a reference emulator. It compares 4×4 constant-colour interiors
against the production decoder/rasterizer, isolating colour behavior from edge
coverage and gradient interpolation. The 326 cases include flat/gouraud quads,
flat/gouraud lines, sprites, all three texture depths, raw/modulated textures,
dithering on/off, opaque drawing and all four blend modes. Additional colours
exercise black modulation, thresholds and saturation. No game bytes are used.

The DuckStation run uses software rendering at native resolution, with true
colour, scaled dithering and PGXP disabled. Its GDB-driven CPU routine waits
for GPUSTAT's ready-to-read bit before reading pixels. Use a dedicated instance:
the test restores CPU registers and its RAM buffers, but its GPU state remains
synthetic until reset. The Redux backend saves/restores a complete emulator
state and reads the renderer's VRAM snapshot directly.

```sh
tools/docker.sh make build/tests/raster_psx_probe test-raster test-gpu
# Start the dedicated DuckStation instance with its GDB server enabled.
python3 tools/test_raster_psx.py --backend duckstation --port 19000 \
  --out build/validation/colour_reference.json
```

The verified run is `build/validation/raster_colour_duckstation_first.json`:
all 326 cases match. Replaying the same packets/reference pixels through an
archived copy of the pre-change renderer gives 253/326 matches
(`raster_colour_duckstation_before.json`). All 73 corrected cases enable
dithering. No undithered cases regress.

`make test-raster` retains independent synthetic golden grids from this
comparison. It checks all sixteen dither origins, modulated palette textures,
lines and the undithered raw/sprite exclusions. Its existing 4096-primitive
generic/optimized differential and region/shadow tests now toggle dithering.
Both the raster and GPU environment/ordering checks pass under ASan/UBSan.

The initial exploratory reports named `raster_colour_baseline*` are not passing
validation: early versions omitted the native setup fill or read GPUREAD before
it was ready. They were retained as diagnostics. The final DuckStation run and
the corrected native setup are the evidence for the counts above.

## Reference limits

The installed Redux software renderer disagrees with DuckStation and the
documented dither eligibility in several flat-textured, raw/gouraud and line
cases. Its dithered blend path also differs. The renderer is not changed to
reproduce those discrepancies. Redux remains useful for original game-code
traces and motion progression; use the controlled DuckStation comparison for
these colour rules.

These tests establish the sampled colour pipeline, not complete GPU or game
parity. Exact polygon edges, varying interpolants, texture feedback, full
animations and audio require separate validation. The historical rectangular
patches around Zidane are not attributed to dithering by this change. Native
performance is not established by these host tests.

## Native DS check

The archived `build/validation/ff9ds_trance_dither` ROM/ELF uses the production
software renderer and the synthetic full-gauge Zidane fixture. The 60-second
`ds_trance_dither_replay.mp4` captures ordinary trance entry, its flash/rays,
the transformed model and subsequent enemy magic. Six motion samples were
reviewed against the earlier original-game trance recording for progression;
the colour-accuracy evidence is the controlled DuckStation test, not an
unsynchronized whole-scene comparison with Redux.

The replay checkpoints bracket game frames 923–2531. The latter has zero
exception/first-fault markers and no protected-code changes in linked
ITCM (`ds_trance_dither_replay_after_inspection.json`). Nine changed data bytes
belong to the IRQ callback table and thread count; the linked code is unchanged.
Both DS and PC builds
pass. An earlier late recording only captured Game Over and was retained as
`ds_dither_gameover.mp4`; it is not animation coverage.

`capture_ds_at_frame.py --state-after PATH` now captures the endpoint checkpoint
automatically after recording, alongside its existing pre-recording
`--state-out`. This avoids a delayed manual checkpoint drifting into a later
scene. Video and checkpoint paths must be distinct.

## Captured game commands and candle review

`tools/replay_gpu_dump.py` replays original game GPU commands through the
production `DrawPrim` / `LoadImage` entry points and software rasterizer,
then compares all 1024x512 VRAM pixels against DuckStation's software renderer.
The GPUDUMP capture supplies initial VRAM and drawing state, eliminating
different game logic, animation phases and camera positions as explanations
for differences. Captures contain game data and must remain in ignored `build/`.

The parser follows the [GPUDUMP format](https://github.com/ps1dev/standards/blob/main/GPUDUMP.md).
It joins GP0 commands across packet boundaries, including uploads; GP1 reset
cancels incomplete commands. Read/discard packets consume GPUREAD words.
Unknown metadata packets are skipped by length. Truncated streams, incomplete
selected frames, interlacing and unsupported GPU revisions are rejected. This probe does not model
GPU timing, scanout, interlaced drawing, or texture-cache behavior on hardware.
It also does not exercise the game's port-specific packet generation.

Use a dedicated DuckStation instance: replay overwrites game RAM, and parks its
CPU afterward. Configure Software rendering, Interpreter CPU, true colour,
scaled dithering and PGXP disabled, with GDB on port 19000. The transport waits
for GPU FIFO readiness before writes and read-transfer readiness before reads.
Without FIFO backpressure, long traces lost commands and readback stalled;
those failed exploratory runs are not fidelity evidence.

```sh
tools/docker.sh make build/tests/gpu_replay test-gpu
FF9DS_DOCKER_ARGS='--network host' tools/docker.sh python3 tools/replay_gpu_dump.py \
  build/validation/psx_candle_current.gpd --frame 240 \
  --out build/validation/candle_replay_frame240
# Recheck a renderer change without running the reference emulator again:
tools/docker.sh python3 tools/replay_gpu_dump.py \
  build/validation/psx_candle_current.gpd --frame 240 \
  --reference build/validation/candle_replay_frame240 \
  --out build/validation/candle_replay_frame240_repeat
```

Frame 0 means the restored preamble; N means immediately after N recorded
VSyncs. Each output directory contains two raw little-endian RGB15/mask VRAM
images and a JSON report. Reusing a reference requires an identical SHA-256 of
the normalized command stream. Exit status 1 reports pixel differences.

The current candle trace has 1199 recorded VSyncs. Initial VRAM matches exactly
(524,288 pixels). After VSync 2 there are 540 differing pixels, located at
x=468..498, y=87..150 in the other framebuffer, around Zidane. At VSync 120
there are 1158 differences, and at VSync 240 there are 1163. These counts include
both framebuffers and textures, not just the currently displayed image. All
differences in these samples also differ in RGB15, rather than only mask bits.
The repeat against the saved VSync-240 reference reproduces 1163 differences;
a second full DuckStation replay also reproduces byte-identical reference VRAM.
These identified the interpolation errors corrected below. The 326
constant-colour interior cases still pass
after extracting the shared DuckStation transport into `duckstation_gpu.py`.

Separately, `ds_candle_current_review.mp4` records 60 seconds of the native
software renderer, from the candle prompt through the round light reveal to
name entry. Its exact ROM/ELF is archived as `ff9ds_candle_current`; checkpoints
bracket frames 1811–3057. The endpoint has no first-fault/exception markers or
protected ITCM changes. Nine changed data bytes belong to IRQ callbacks and
the cothread count. The current DS and host sequences show no historical
rectangular patches around Zidane. The original `psx_candle_current_reveal.mp4`
shows the same progression. This scoped visual review does not establish
pixel parity or close the remaining interpolation errors above.

The scenario enters field 70, then original scripts transition to field
50; both the PSX prompt state and native name-entry state have active field 50.
An initial check expecting active field 70 was incorrect. An unrelated
DuckStation slot showing Vivi/Puck in Alexandria was discarded as a candle
reference. Native performance and whole-game fidelity remain unverified.

## Polygon interpolation correction

The software rasterizer now seeds RGB/UV with a half-unit bias, quantizes
attribute slopes to twelve fractional bits with truncation toward zero, and
evaluates the starting pixel from the leftmost input vertex using those same
slopes. Previously it floored interpolants and computed the starting value
independently of its stepped gradients. Quads now use strip order 0,1,2 / 2,1,3;
reversing the second triangle changes its attribute seed when X coordinates
tie, despite covering the same geometric area. These behaviors agree with
controlled draws through the [DuckStation software GPU](https://github.com/stenzek/duckstation/blob/master/src/core/gpu_sw_rasterizer.inl).
This is an independent edge-function implementation, not a replacement with
the reference emulator's scanline rasterizer.

The arithmetic retains the existing 16.16 stepping representation, with four
low slope bits zero. It uses signed 32-bit division: byte attributes and the
GPU's accepted extent limits bound the scaled determinant below 2^31.
Maximum-size triangles with determinant +/-1 exercise setup and wrapped
stepping under UBSan. The generic/optimized differential still checks 4096
mixed primitives and shadow-region rendering; ten independent synthetic PSX
goldens now run on both host paths and all three ARM9 benchmark backends.

`test_raster_psx.py` now compares 1006 cases. Added cases use patterned 4/8/15
bit textures, varying colours, negative/clipped coordinates, all triangle
vertex permutations, flat/gouraud quads, tied X coordinates, raw/modulated
textures and dithering. The reference starts with fully cleared VRAM, including
unuploaded texture regions. Its packet staging buffer was enlarged, with an
overlap guard, to fit the additional synthetic uploads.

`raster_interpolants_full_clear.json` matches **1006/1006**, versus **358/1006**
for the archived pre-change renderer (`interpolation_final_baseline_counts.json`).
Provisional gradient reports packed V at bit 16 instead of bit 8; corrected
cases use byte-sized U/V fields. Eight exploratory mismatches sampled unused
reference VRAM containing game leftovers; explicitly clearing it removes
that setup difference. Eight deliberate page-boundary cases retain this
coverage with deterministic memory. The final full-clear report is the
authoritative synthetic comparison.

Captured-command checks compare all 524,288 VRAM pixels, including mask bits:

| Recorded VSync | Before differing pixels | After |
|---|---:|---:|
| 180, room reveal | 1146 | 0 |
| 240, candle sequence | 1163 | 0 |
| 330, fade toward naming | 7 | 0 |

Reports are `candle_replay_interpolants_room`, `candle_replay_12bit_strip`, and
`candle_replay_interpolants_revealed`. The final 32-bit arithmetic repeats the
VSync-180 zero-difference result in `candle_replay_interpolants_final32_room`.
At VSync 330 the viewport is nearly black during a fade, so it is not a useful
standalone room-reveal screenshot. VSync 180 was visually checked during the
light reveal. Extending the replay through name entry reaches a GP0 readback
after VSync 330 and is explicitly rejected by the current tool; no complete
1199-VSync replay match was claimed at this stage. Other scenes, varying
line interpolation, audio and whole-game correctness still require work.

The native `interpolation_timing` ROM includes both old and new ARM/ITCM
backends in the same run. `interpolation_timing_32bit.png` confirms that the
PSX goldens and new paths match. All ten goldens and seven full-image comparisons pass.
Its guest-timer comparison ranges from approximately 0.1% faster to 6.1% slower
for the new backend across seven cases; it does not establish game FPS.
Different texture samples also change the work per pixel. This remaining
cost is recorded in `interpolation_arm9_validation.json`; rendering fidelity
improves, but performance and full-game fidelity are not finished.

## Readback replay and odd image padding

The replay now consumes GPUDUMP types 3 and 4 as read counts. Type 3 means
read-and-discard, not ignored GPU input; the earlier parser interpreted it
incorrectly. Both types are compared with the port's `StoreImage` output,
including words that a normal dump player would discard. Split read batches and
GP1 reset cancellation are supported. GP1 writes bypass the GP0 FIFO wait so
reset can cancel an incomplete transfer. Reads past
the end of a VRAM transfer, interlacing, and a selected checkpoint with an
unfinished read remain explicit errors. Zero-length read batches are no-ops.
These semantics follow the [GPUDUMP specification](https://github.com/ps1dev/standards/blob/main/GPUDUMP.md).

Output now also includes `native.reads` and `psx.reads`, with concatenated
little-endian GPUREAD words in stream order. `different_read_words` participates
in the exit status, so matching final VRAM cannot conceal wrong readback data.
Reference reuse checks the same normalized command hash, including read batches.

The complete candle capture replays through all 1199 recorded VSyncs and ends
on the naming screen. `candle_replay_readbacks_complete` and the sentinel-buffer
repeat `candle_replay_readbacks_repeat` both have zero final VRAM differences
and zero differences in the 64 words returned by the transition's one VRAM
read. The latter contains 686773 normalized records and 2529361 register writes.
The naming framebuffer was viewed. This compares the final VRAM and recorded
read results after the whole stream; it does not compare every intermediate
VSync or establish native DS scanout/scaling fidelity.

Synthetic even-sized transfers also match across both VRAM axes, with partial
read batches using both read/discard packet types (`readback_even_first`).
Reset during a partial read followed by a new transfer also matches reference
VRAM and both returned words (`readback_cancel_verified`); a zero-length batch
in that trace does not block or consume data.
An odd three-pixel read matches the GPU words `22221111 00003333`. Initially,
zero-initializing the native buffer hid a `StoreImage` defect: the API copied
three halfwords but left the fourth stale. Changing the probe buffer to
`DEADBEEF` exposes one differing word (`readback_odd_sentinel_before`). The
production API now zeros that padding halfword, matching the reference
(`readback_odd_sentinel_fixed`). The original PsyQ transfer helper at 80014AA8
also computes `(w*h+1)/2` words and writes full words to the destination.

`test-gpu` now checks an odd 3x3 upload/read across x=1023 and y=511. It verifies
upload padding is discarded, read padding is zero, all nine pixels survive,
and adjacent caller words remain untouched. Parser tests cover partial reads,
reset cancellation, zero counts and malformed/unsupported operations. Both
DS and PC targets build. This closes the recorded naming readback and the odd
padding defect; it does not establish complete image-transfer, GPU or game parity.

## Battle capture, register queries and transfer masks (2026-10-16)

`tools/capture_psx_gpu.py` records GPUDUMP and an emulated-time video from an
already paused Redux instance. Optional state restore warms a cold BIOS first.
Before/after screenshots and status metadata retain the capture's provenance;
all output must remain in ignored `build/`. For example, with disc 3 mounted:

```sh
python3 tools/capture_psx_gpu.py --state ds_ifrit_full_model_ready --seconds 18 \
  --out build/validation/ifrit_gpu_reference
```

The saved summon state produces an 18-second recording and 1079 recorded
VSyncs. Its contact sheet was reviewed: initial flash, Ifrit's model, raised
arm, fire effect and return to battle are present. This is motion/context
evidence; the Redux software renderer has the colour limitations described
above. Numerical drawing comparisons use DuckStation software rendering.

Battle calls GPU drawing-register queries every VBlank. Replay now models
the GPUREAD latch, using production `GetDrawEnv2` to snapshot GP0 state. Queries
2/3/4/5 return texture window, clip corners and offsets; changing GP0 registers
alone does not update a latched response. Immediate query reads are batched
with register writes and do not wait for the VRAM-read ready bit. The
11-word `gpu_query_drawing_latches_verified` fixture matches, including repeat
reads, signed offsets, aliases and retained values.

The reference has additional query limitations: DuckStation's GPU frontend
masks indices with 7 and leaves indices 7/8 effectively unchanged, rather than
the documented v2 version/zero results. `gpu_query_latches_first` retains four
exploratory mismatches for these cases; it is not passing calibration of those
indices. See the [GPU query specification](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp110h-read-gpu-internal-register)
and [reference frontend](https://github.com/stenzek/duckstation/blob/master/src/core/gpu.cpp).
The captured battle queries only indices 2–5. Other GPU revisions and 2 MiB
VRAM metadata are explicitly rejected.

| Ifrit recorded VSync | Final VRAM differences | Returned words compared |
|---|---:|---:|
| 240, model entrance | 0 | 960 drawing-register words |
| 840, fire effect | 0 | 3360 drawing-register + 15360 image words |

Both checkpoints compare the full 524288-pixel VRAM, including mask bits.
Reports are `ifrit_replay_model` and `ifrit_replay_fire`. The latter includes
120 image reads and still matches after the transfer changes below
(`ifrit_replay_fire_masks_fixed`). These are checkpoints reached through the
recorded stream, not comparisons of every intermediate frame or native game
packet generation.

### Transfer and packet fixes

`LoadImage` and VRAM copies previously ignored E6 force-mask/protect-mask bits.
They now preserve protected destinations, force bit 15 when requested, and
consume every incoming upload pixel even when its destination is protected.
Masked writes materialize deferred destination pixels before testing their
bits. Identity copies still skip work when force-mask is disabled; with
force-mask enabled they update unprotected pixels. The low-level copy command
also decodes zero width/height as 1024/512, while an empty high-level RECT
keeps its existing no-op behavior (`gpu_copy_zero_dimensions_verified`).

The packet walker also ran all fills/copies before decoding a packet's drawing
commands. It now preserves command order, including mask changes preceding a
copy, and a primitive followed by a copy and clear. Hardware fill callbacks
still execute. Tests verify the copied primitive survives and the original
source is cleared, rather than drawing after the clear.

Transfer references must be interpreted per path. The four-pixel mask fixture
has 20 differences before the fix. Afterward, four remain against DuckStation
software because its scalar upload tail advances the source pointer only for
unprotected destinations. Its vector upload path and the port consume every
source pixel; the 32-pixel fixture matches all flags (`gpu_transfer_masks_wide_fixed`).
DuckStation Vulkan agrees with the port's four-pixel uploads, but differs on
two protected copy destinations when force-mask and protect-mask are both set
(`gpu_transfer_masks_vulkan_fixed`). The software copy agrees with the port.
The port follows mask protection rather than reproducing either reference bug.
`test-gpu` retains explicit upload/copy goldens and checks destination
materialization, source consumption and identity copies. Reference scalar
behavior is visible in [WriteVRAMImpl](https://github.com/stenzek/duckstation/blob/master/src/core/gpu_sw_rasterizer.inl).

`make gpu-ds` runs the production transfer tests on ARM9 without game data.
The archived `gpu_transfers_final_arm9` ROM/ELF checks masks, odd padding,
wrapping, maximum-size zero dimensions and packet order.
`gpu_transfers_final_arm9_window.png` reports PASS; an earlier all-black root
capture was caused by the private X server's screen saver, not a failed test.
The host candle route also reaches frame 2800/name entry
normally (`candle_gpu_masks_host.log`), with its light reveal and naming
screens visually reviewed. DS/PC builds and GPU/GX regression suites pass.
Overlapping-copy buffering, further effects, native animation fidelity,
audio and whole-game coverage remain open.

## Published overlapping-copy reference (2026-10-16)

The port reproduces all 147 cases in the published
[ps1-tests VRAM overlap test](https://github.com/JaCzekanski/ps1-tests/tree/93568c5589e58b1fe442c854728324994bd30426/gpu/vram-to-vram-overlap).
Its seven rows cover block sizes 2, 8, 15 and 16, with the last three rows
offsetting the source by one pixel in X, Y or both. Each row tests shifts
X=-3..3 and Y=-1..1. Initial patterns are uploaded with `LoadImage` and copied
through production `MoveImage`.

All 123627 pixels in 29x29 cell interiors match the published `vram.png` in
RGB15 (`overlap_reference_baseline.json`). These interiors omit labels and
grid lines; no rasterized font or line behavior is inferred. The PNG SHA-256
is `28cd11886418438e76fafd970f990205ffe6fd776e755b846ab427fa86c305af`.
The image's console model/capture provenance is not specified, so this is
agreement with a published reference, not a new physical-console measurement.
The image also cannot verify bit 15. Wrapping overlaps and protected-pixel
interactions remain separate questions.

`tests/gpu/overlap_reference.h` retains the synthetic input layout and 147
independent reference hashes. Each checksum starts at 2166136261 and updates
as `(hash ^ rgb15) * 16777619` modulo 2^32 in row order. The MIT attribution
and license are retained alongside the test. `make test-gpu` verifies these
goldens under ASan/UBSan; `make gpu-ds` runs the same cases on ARM9. The archived
`gpu_overlap_arm9` ROM/ELF and screenshot report all 147 cases passing, followed
by the existing transfer/mask/packet checks. Both game targets build.

No overlap algorithm change was needed for these cases. The source comment
now states the tested scope instead of claiming general overlap fidelity.
