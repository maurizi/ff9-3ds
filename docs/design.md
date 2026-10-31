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
`DrawOTag` walks the OT in PS1 order as before (`port/psx/gpu.c`, `gp0.c`), and each decoded primitive becomes
PICA200 geometry, drawn in OT order (painter's algorithm, as on the PS1). What the PICA200 adds:
- **Exact blending.** PS1 modes 0–3 are GL blend equations: B/2+F/2 (constant alpha ½), B+F (one, one), B−F
  (reverse subtract), B+F/4 (constant colour ¼). Semi-transparent textures draw in two passes split by the
  texel's semi-transparency bit (opaque texels, then blended ones), as on the PS1.
- **Textures.** The PICA200 has no palettes, so CLUT pages are converted to RGBA8 textures (alpha encodes
  transparent / opaque / semi-transparent texels) by a cache keyed on (page, depth, CLUT), converted lazily in
  32×32 blocks and invalidated by VRAM writes (`psx_vram_touch`). The CPU keeps the 1 MB VRAM shadow for
  uploads, CLUTs and readbacks.
- **Depth from the GTE.** The software GTE records, for every screen coordinate it stores, its sub-pixel
  position and its depth (SZ), keyed by the address it was stored to (§4.3). A primitive whose vertices all
  have one is drawn with sub-pixel precision (no PS1 wobble), perspective-correct texturing, and **stereoscopic
  parallax**: a camera moved by e along X sees the vertex at sx − H·e/Z, so each eye's view is exact for
  every projected vertex, effects included. Primitives without depth (2D, UI) sit at the screen plane.
- **Render targets.** The PS1 draws into VRAM rectangles (two display buffers, offscreen effect areas). Each
  drawing area the game uses gets a GPU render target; the displayed one is presented; one sampled as a texture
  (screen-capture effects) is used as one directly; a CPU readback (`StoreImage`) downloads it.

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
- **Top**: the scene (field, battle, world map, FMV), 400×240 per eye, the PS1 frame at 1:1 horizontally and
  ×1.07 vertically in 4:3, later widescreen.
- **Bottom**: the game's UI at 1:1: battle commands and party status, menus, dialogue choices, the world map's
  minimap, Tetra Master's board; touchable (§6).

Primitives are routed to a screen by what drew them: the game's UI drawing paths (windows, text, cursors) are
identified in the decomp and tagged, and a tagged primitive goes to the bottom screen's render target.

## 5. Sound
AKAO (the game's sequencer) runs as decomp C against the emulated SPU register file (`port/game/spu_regs.c`), as
in ff9-ds. The exact software SPU (24 voices, Gaussian interpolation, ADSR, reverb; `port/psx/spu_*.c`) mixes at
44.1 kHz and NDSP plays the mix. The mixing moves to the second CPU core (a thread on the system core on Old
3DS, a full core on New 3DS) once it measures as a cost on the game thread. Voice acting (optional) streams Opus
files from the SD card, keyed by dialogue text id.

## 6. Input and UX
- Buttons: A = ✕ (confirm), B = ○ (cancel), X = △ (menu), Y = □, L/R = L1/R1, ZL/ZR (or L+R) = L2/R2, Start,
  Select; the circle pad doubles the d-pad (FF9 reads a digital pad).
- Touch: every bottom-screen UI is tappable (list rows, commands, targets, the Tetra Master board) by writing
  the game's own cursor state or synthesising the button the game expects; buttons always work too.
- Suspend/resume through the HOME menu and sleep mode; saves on the SD card (memory-card images).

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
