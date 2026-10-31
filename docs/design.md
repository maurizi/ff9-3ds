# ff9-3ds architecture

A native 3DS port of Final Fantasy IX (PS1, NTSC-U), built from the C source of the ff9-decomp matching
decompilation. It started as a fork of ff9-ds (the DS port), which supplies the decomp→native build pipeline, the
PS1 library layer and the host build. Decisions made there under DS limits (4–16 MB, 2048 polygons, no blend
modes, 256×192 screens) are not carried over; this document is the 3DS design.

## 1. Goals

1. **A native port, not an emulator.** Game logic is the decomp's C, compiled for the ARM11. Hardware access goes
   through a PsyQ-compatible layer only where the game's own code expects it, and the presentation is the 3DS's:
   the PICA200 draws the scenes, the game's UI lives on the touch screen, and the two screens are used together.
2. **Performance of a native game**: the PS1's frame rates (fields 30, battles 15 and the world map 20 scene
   updates per second, in 60 Hz VBlanks) with no slowdown on an **Old 3DS**; the New 3DS gets the optional
   extras (§8).
3. **3DS features**: stereoscopic 3D everywhere 3D is drawn, dual-screen UI with touch, and later optional
   cel-shading, voice acting, high-resolution models and StreetPass.
4. **No game data in the repository.** Everything is derived from the user's discs at build or run time.

## 2. Hardware

| | PS1 | Old 3DS | New 3DS |
|---|---|---|---|
| CPU | R3000A 33.9 MHz + GTE | ARM11 MPCore 268 MHz ×2 (app core + ~30% of the system core) | 804 MHz ×4 (app gets 2-3), 2 MB L2 |
| RAM | 2 MB + 1 MB VRAM | 64 MB FCRAM (≈ 40–64 MB to an app), 6 MB VRAM | 124/256 MB, 6 MB VRAM |
| GPU | 2D rasteriser, 4 blend modes, 15/24-bit | PICA200: vertex shaders, depth/stencil, 6-stage combiners, every GL blend equation, RGBA8/5551/565/4444/ETC1/L8/A8 textures (no palettes) |
| screens | 320×224/240 (4:3) | top 400×240 (stereo, 800×240 in wide mode), bottom 320×240 touch |
| sound | SPU: 24 ADPCM voices, reverb | DSP: 24 channels (PCM8/16, DSP-ADPCM), NDSP; software mixing affordable |

Two facts shape the design. **The bottom screen is 320 pixels wide, the PS1's width**: any PS1 UI can be shown
there 1:1, pixel for pixel, and touched. **4:3 at 240 lines is 320×240**: the top screen shows the PS1's frame at
its own horizontal resolution with 40-pixel margins, which a widescreen option can fill with real scene (fields
whose backgrounds are wider than the view, and the 3D modes).

## 3. Code: decomp C → ARM (inherited)

Unchanged from ff9-ds (`docs/porting-notes.md`): `tools/portify.py` copies each decomp file into
`build/portified/` with PS1 addresses mapped; a forced-include prelude replaces the GTE macros and hardware
access; `tools/gen_link.py` builds the data images from the user's executable, namespaces the overlays (they all
stay linked: memory is plentiful) and writes the resident-overlay trampolines; link-time overrides
(`config/overrides.txt`) replace game functions the port reimplements. PS1 data lives at its PS1 address in
`psx_ram[]`, a 2 MB array inside the executable's image, so 24-bit OT links stay valid pointers.

Build: `tools/docker.sh make 3ds` (devkitARM, libctru, citro3d; ARMv6K ARM state, hard float) →
`build/ff9.3dsx`. `make pc` is the 32-bit host build of the same code for fast iteration and frame dumps.

## 4. Rendering

The game builds GPU packets in ordering tables (OTs) with screen-space coordinates from the GTE. The renderer
replaces the PS1 GPU at two levels:

### 4.1 The primitive renderer (all packets, `port/3ds/render.c`)
`DrawOTag` walks the OT in PS1 order (`port/psx/gpu.c`, `gp0.c`), and each decoded primitive becomes PICA200
geometry, recorded as batches in drawing order (painter's algorithm, as on the PS1) and replayed at the end of each
ordering table.
- **The VRAM mirror.** All of PS1 VRAM is a 1024x512 RGBA5551 render target per eye: GPU coordinates are PS1 VRAM
  coordinates, so double buffers, offscreen passes and screen captures land where the PS1 drew them. The alpha bit is
  the PS1's **mask bit**: draws write it as the PS1 does (set_mask, else a textured pixel's texel bit 15, untextured 0;
  fills clear it), opaque draws with check_mask leave masked pixels alone (destination-alpha blending), uploads carry
  it. The CPU keeps the 1 MB shadow for uploads, CLUTs and readbacks; per 32x32 cell either side may be stale: a CPU
  write is copied up (only the stale cells) before anything draws over it or it is shown; a CPU read of a cell the
  GPU drew reads it back first (bit 15 from the alpha).
- **Exact blending.** PS1 modes 0-3 are GL blend equations: B/2+F/2 (constant ½), B+F (one, one), B−F (reverse
  subtract), B+F/4 (constant ¼). Semi-transparent textures draw in two passes split by the texel's bit 15 (opaque
  texels, then blended ones), as on the PS1.
- **Textures.** No palettes on the PICA200: CLUT pages become RGBA8 textures (alpha: transparent / texel without bit
  15 / with it) keyed by (page, depth, CLUT, texture window), converted lazily in 32x32 blocks. A block is checked
  against the cells it was made from (a texture epoch skips the check when nothing it used was written), then by a hash
  of its source texels and CLUT, so the world map's per-frame palette cycling and water scrolling reconvert only what
  changed. Texture windows (GP0 E2) are baked into their own slots. A slot drawn with earlier in a frame forces a GPU
  sync before it changes.
- **Depth from the GTE** (§4.3): sub-pixel positions, perspective-correct texturing and stereo parallax for every
  projected vertex; a primitive uses depth only if all its vertices have it. 2D primitives in the scene get the depth
  of their ordering-table entry (a per-frame fit of SZ against entry over the projected ones): layered stereo for
  field backgrounds. Full-screen ones stay at the screen plane.
- PS1 rules kept: quads are two triangles, each skipped when 1024+ wide or 512+ high.

Debugging: `DUMP_AT=N` (mirror, shadow, mask plane, texture slots to the SD card), `DUMP_EVERY=N` (the displayed
frame every N frames, UI composited), `PROBE_AT=N` (GP0 commands, fills and transfers around a frame), and
`tools/render_diff.py`, which runs a scenario on the host's software renderer and on the 3DS (`SPUSYNC=1` keeps the
two in step) and compares them frame by frame.

### 4.2 Native scene renderers (replacing the game's per-vertex work)
Where the game's own functions are known (the DS port identified most of them: `config/overrides.txt`), the
port takes the geometry before projection and draws it with the PICA200's vertex shaders and depth buffer:
- **Field backgrounds**: layers as textured quads at their layer depth (correct stereo depth per layer; per-pixel
  occlusion of characters by the depth buffer instead of OT buckets).
- **Models** (field, battle, world map): vertices transformed on the GPU from the game's bone matrices; the
  inputs to cel-shading (normals) and to replacement high-resolution models.
- **World map terrain**: blocks as vertex buffers.
These replace the generic path one domain at a time; the generic path stays the fallback and the reference.

### 4.3 GTE depth capture
`gte_stsxy*` (and the port's own projection overrides) write, beside the 32-bit SXY word, a shadow entry
`{sub-pixel x, y, SZ, the word stored}` for its address (a parallel array over `psx_ram` and the scratchpad).
The GP0 decoder reads each vertex word's entry by address and uses it only when its stored word still equals the
vertex word, so packets the game rewrites or builds by hand simply lose depth. Projected vertices copied
through the scratchpad are found through a per-frame table keyed by the word's value.

### 4.4 Screens
- **Top**: the scene (field, battle, world map, FMV), 400×240 per eye, the PS1 frame at 1:1 in 4:3, both edges
  masked by the largest parallax in stereo (a floating window).
- **Bottom**: the game's UI at 1:1 over a dimmed copy of the scene. UI is: the battle's VBlank list
  (`ContinueDraw`), and every packet the window system made (`win_drawAll` is wrapped: `port/game/ui_split.c`
  reports the packet range it filled). Those draw into one of two UI layers (one per PS1 framebuffer: a field's
  windows are drawn into the frame being built), RGBA8 with premultiplied alpha, which composites PS1 blend modes 0,
  1 and 3 over the scene exactly. While a menu is up (`GD_MENU_OPEN`, all of the frame once `GD_HIDE_ALL`), the whole
  frame is UI and the top shows the scene as it was when the menu was asked for. The card game (mode 9) is shown on
  both. In a field or on the world map with no window up, the bottom screen is the **field panel**: a drag pad and the
  face buttons, drawn with the game's own icons.

## 5. Sound and threads
AKAO (the game's sequencer) runs as decomp C on the game thread against the SPU register file it reads. The exact
software SPU (24 voices, Gaussian interpolation, ADSR, reverb; `port/psx/spu_*.c` built as `spuw_*`) runs on a
thread on the other core (core 2 of a New 3DS, the system core at up to 80% on an Old 3DS), fed through a ring of
register writes, sample advances and SPU RAM transfers (`port/3ds/spu_client.c`, the DS port's spu7 protocol); what
the game reads back comes back after each advance, at most one AKAO tick late. NDSP plays its mix. Movies: the MDEC
decodes each frame on a worker on the same core as soon as its run-length data is complete (`port/game/movie.c`).
Frames are paced by citro3d's VBlank counter. Voice acting (optional) streams Opus
files from the SD card, one per dialogue line (text bank, message number), on its own DSP channel with the music
ducked (`docs/voice-packs.md`).

## 6. Input and UX
- Buttons: A = ✕ (confirm), B = ○ (cancel), X = △ (menu), Y = □, L/R = L1/R1, ZL/ZR (or L+R) = L2/R2, Start,
  Select; the circle pad doubles the d-pad (FF9 reads a digital pad).
- Touch (`port/3ds/touch.c`): FF9 points at the selected entry with a hand; the renderer records where it is drawn.
  A tap steers the game's own cursor there with d-pad presses (rows, then columns; cells in the card game) and
  confirms; swipe left is back, swipe up/down scrolls. The game's cursor logic is never bypassed. The field panel's
  buttons hold their PS1 button; its drag pad is the d-pad.
- Saves: two memory cards, raw `.mcr` images (`port/psx/card.c`): emulator and PS1-card saves load, and the port's
  saves work in emulators. Disc changes: the disc the game's own screen asks for goes in the drive.
- Suspend/resume through the HOME menu and sleep mode (libctru's applet handling).

## 7. Data
`sdmc:/3ds/ff9/`: `raw/FF9 CD1.img`…`CD4.img` (or a packed `discs.bin`), cards, and later optional asset packs
(voice, high-resolution models). The executable carries only code and the relocation images built from the
user's executable at build time.

## 8. Old and New 3DS
Everything in §1–7 must run at full speed on an Old 3DS. The New 3DS's 804 MHz, L2 and extra cores pay for
extras: cel-shading outlines (an extra pass), high-resolution replacement models, 60 Hz interpolation.

## 9. Testing
- Host: `make pc`, `--scenario` fixtures (`docs/scenario-testing.md`), frame dumps.
- Emulator: `tools/azahar.sh` boots a build in Azahar on a private X display, captures screenshots and the
  game's log (`svcOutputDebugString`). `make 3ds SCENARIO=sdmc:/3ds/ff9/scenarios/X.cfg PROFILE=1` boots
  straight into a fixture with per-category timers.
- Hardware: not yet (an Old 3DS for performance sign-off when available).
