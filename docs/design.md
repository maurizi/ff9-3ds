# Port architecture

## 1. Starting point (ff9-decomp as of 2026-10-11)

| | state | what it means for the port |
|---|---|---|
| game C | 3821/3837 functions match (98.8% of bytes); 15 of the 20 binaries are complete | nearly all game logic compiles as-is |
| left as asm | 16 game functions (drafts exist in `docs/wip/`), ~150 hand-written asm routines, 542 PsyQ/BIOS functions | the port must supply C for every routine it calls. See §4 |
| names | about 2% of functions and 5% of globals have names | the port can't wait for names. It hooks at PsyQ/hardware boundaries, which are already named, and asks for names only where the dual-screen UI needs game semantics |
| binaries | the exe plus 19 overlays. Six overlays share 0x800A7000 and thirteen share 0x801EDF00 | overlay symbols collide; see §3.3 |
| data | `.data`/`.rodata` exist only as splat `.s` generated from the user's exe | the port generates its data from the user's exe at build time; see §3.4 |

Hardware coupling in game C, counted in `src/`: ~960 GTE macro uses (73 distinct macros), 335 `addPrim`, ~400
textured polygons/sprites set up by hand, 88 `LoadImage`, 34 `StoreImage`, 30 `MoveImage`, direct SPU register
pokes in the AKAO driver (`SPU_REG16`, `SPU_VOICE_REG`), scratchpad pointers (`0x1F800000`), and 33 distinct
hard-coded RAM addresses (`0x801EDF00`, `0x801FCB00`, ...).

## 2. Target hardware

### Original timing baseline

Preserve the original NTSC-U timing before considering interpolated 60 FPS
scene rendering. In the matching decompilation, field initialization
(`ovl_0a`, `func_800A8198`) sets `g_gameState->unk9 = 2`, battle initialization
(`ovl_0b`, `func_800A8CD4`) sets 4, and world-map initialization
(`ovl_0c`, `func_800A8B14`) sets 3. These correspond to approximately 30, 15,
and 20 scene updates per second at 60 Hz, respectively. These are scheduling
intervals, not guarantees against slowdown. The world-map presentation function
`func_800AC5C8` selects 2, 0 or 3 according to state; do not cap all world scenes
at 15 FPS. Battle code can also change the interval (`func_800E8F40`).

VBlank callbacks, input, music timers and sample production must continue at
their original cadence between scene updates. The battle UI inserts GPU work
through `BreakDraw`/`ContinueDraw` (`func_800F4A00`); its exact element-by-element
update rates still need synchronized PSX recordings. The PSX has no separate
hardware text overlay plane. Count scene submissions and UI updates separately
from virtual VBlanks, DS scanout and emulator wall-clock FPS. A lower scene
update rate must not slow game time or reduce audio production.

### Hardware budget

**Working baseline: DSi mode**, with a 134 MHz ARM9 and 16 MiB main RAM.
**Preferred hardware target: original DS mode**, with a 67 MHz ARM9 and 4 MiB main RAM.
The current game build does not fit the original DS budget. New architecture work
must account for that budget now; see §8, `ds-mode.md` and `decisions.md` D1.

| resource | PS1 | DSi mode | notes |
|---|---|---|---|
| CPU | R3000A 33.9 MHz + GTE | ARM946E-S 134 MHz (+ ARM7 33 MHz), 8 KB D-cache, 16 KB I-cache | the GTE becomes software; the DS geometry engine can take over hot paths later |
| RAM | 2 MB | 16 MB | current build keeps all native overlays resident, plus PSX RAM, VRAM and SPU shadows; see §8 |
| scratchpad | 1 KB | DTCM 16 KB, ITCM 32 KB | scratchpad → DTCM; the GTE core and the OT walker → ITCM |
| VRAM | 1 MB, framebuffers included | 656 KB in banks; the 3D engine writes the screen directly | 512 KB for the current GX path; planned effects reduce this budget; see §6 |
| GPU | ~ 90k–180k polys/s, 4 blend modes | 2048 polygons / 6144 vertices per frame, alpha blending only, depth buffer, fog, 4bpp/8bpp paletted textures in the PS1's BGR555 | per-frame budget and blending are the main risks; see §5.4 |
| sound | SPU: 24 ADPCM voices, ADSR, reverb, 512 KB | 16 channels (PCM8/PCM16/IMA-ADPCM; 14–15 noise), no ADSR or reverb, 2 capture units | current SPU mixing runs on ARM9; ARM7 streams PCM via Maxmod |
| storage | CD, 4 discs | SD card | everything is on SD; disc swaps resolve instantly |
| display | 320×224/240 | 2 × 256×192, bottom one touch | see `dual-screen.md` |

## 3. Turning decomp C into an ARM binary

### 3.1 How the source is consumed (implemented in M0)
- `ff9-decomp` is a **git submodule** (`decomp/`). The port compiles `decomp/src/**/*.c` and
  `decomp/src_equiv/**/*.c` with `arm-none-eabi-gcc` 16 (BlocksDS) and never edits those files. Port-specific
  behaviour comes from these mechanisms, in order of preference:
  1. **`tools/portify.py`**, a mechanical copy of each file into `build/portified/`. It drops the MIPS rodata
     padding `__asm__` statements (any other inline asm fails the build). It wraps hardware addresses
     `0x1F80xxxx` in `PSX_HW()`, and PS1 RAM addresses `0x80xxxxxx` in `PSX_ADDR()` (§3.5). Exceptions go in
     `config/addr_plain.txt`; there are none yet. The 16 RAM literals not in pointer casts are listed per file in
     `build/portified/**/*.addr.txt`.
  2. **A forced-include prelude** (`port/include/port_prelude.h`). It turns `INCLUDE_ASM`/`INCLUDE_RODATA` into
     nothing and includes the file's own header (`ff9.h` or `ovl_XX.h`). Then `port_hw.h` `#undef`s and
     redefines every hardware macro those headers define, and the file's own `#include` becomes a no-op through
     its include guard:
     - the ~96 GTE macros, translated instruction by instruction into the software GTE's
       `mtc2`/`mfc2`/`ctc2`/`cfc2`/`cmd` (`port/psx/gte.c`), so register state persists between split macros
       the way it does on hardware;
     - `addPrimLen`, the scratchpad stack switch (a no-op), `SPU_VOICE_REG`;
     - `gte_ldrotkey`, which isn't GTE at all: it's a CPU animation-key decode, `port/game/anim_key.c`.
     Where an asm macro has a branch, the C follows the original *bytes* (they're `noreorder`, so delay slots
     always execute: e.g. `gte_stsvclamp`, checked at ovl_0a 0x1460C).
  3. **Port prototypes** (`port/include/port_decls.h`). Some calls come before any declaration, and gcc 2.8
     treated them as `int f()` when the definition is narrower; modern gcc rejects that. `tools/gen_decls.py`
     generates per-binary prototypes from gcc's own diagnostics into `port_decls_gen.h`: 30 so far. One call
     site passes 5 arguments to a 4-parameter function (`ovl_0a func_800C6C04`), which is handled by hand.
  4. **PsyQ replacement**: the game calls PsyQ by name (`LoadImage`, `DrawOTag`, `CdRead`, ...).
     `port/psx/` implements that API, so all 542 library routines go away without game edits.
  5. **Link-time overrides** for game functions that must behave differently on DS (the CD loader, the overlay
     loader, the field BG renderer, UI hooks): `objcopy --weaken-symbol` on the decomp object, so the port's
     definition wins.
  6. **Upstream decomp changes**, only for what 1–5 can't handle, and never changing PS1 codegen.
- Flags: `-std=gnu89 -fpermissive` (K&R-era C; gcc 16 defaults to C23, where `f()` means no arguments),
  `-fsigned-char` (MIPS `char` is signed, ARM's isn't), `-fno-strict-aliasing -fwrapv
  -fno-delete-null-pointer-checks -fno-builtin-memcpy`, `-O2 -mthumb`. Hot code moves to ARM/ITCM later.
- **Structure layout** is the same on both CPUs (little-endian, 32-bit, same natural alignment), so on-disc data
  can be read in place. `tools/layoutcheck.py` (to do) will compare `sizeof`/`offsetof` from DWARF for both
  toolchains, to catch bit-field differences.
- **Ordering tables** keep their 24-bit pointers (`P_TAG.addr:24`). DSi main RAM is exactly the 16 MB at
  `0x02000000`, so the OT walker rebuilds each pointer as `0x02000000 | addr24`.

### 3.2 Remaining asm (decomp side)
The port needs a C body for every non-library routine still in asm. A **functionally equivalent** body is enough.
These live in the decomp's `src_equiv/<binary>/` with the same names and headers, and the port compiles them
like `src/`. Until one exists, the link stubs it (§3.6). See `decomp-requests.md` R1.

### 3.3 Overlays: namespaces for code, slots for data
- Every overlay's code is linked into the one ARM9 binary. `tools/gen_link.py` writes an
  `objcopy --redefine-syms` map per binary. Each overlay function, whether defined in its objects or listed for
  it in the decomp's function index, becomes `oXX_<name>` (`ovl_0b` → `o0b_func_800A859C`). A reference to
  another binary's function is pointed at that binary's name.
- **When the target depends on the resident overlay**, the generator emits a trampoline instead of picking one.
  On PS1, main calling `0x800BDED4` means ovl_0a's function in the field and ovl_0b's in battle. The trampoline
  (`build/gen/tramp.s`) is a 4-instruction-per-candidate ARM veneer: it reads `psx_ovl_resident[slot]` and
  tail-jumps to that overlay's function, using only `ip`, so arguments arrive untouched. It currently covers 4
  references: `func_800A859C`, `func_800BDED4`, `func_800C1A8C`, and the menu table word `0x801F4748`.
  `config/xrefs.txt` can pin a reference to one overlay instead.
- **Overlay "loading"** (`psx_overlay_load(id)`) copies the overlay's data image into its PS1 slot in `psx_ram`
  and marks it resident, so its `.data` is reinitialised exactly as a PS1 reload would. The game's loader is
  overridden to call it for dir-0 code files (M1).

### 3.4 Data: the user's bytes, relocated (`build/gen/img_<bin>.s`)
- `make extract` runs the decomp's own extract and split on the user's discs. `gen_link.py` then takes each
  binary's `.rodata`, `.data` and data inside `.text` (ovl_0b's table) **from the user's file**, byte for byte.
  It uses splat's `.word` lines only as the relocation list, since splat types everything else as
  `.short`/`.byte`/`.asciz`. Each word is classified:
  - a function (by splat's symbol, or a raw value that is a function start) → the native function, Thumb bit set
    by the linker;
  - a raw value inside the overlay's own rodata/data → a data pointer;
  - a raw value in another binary's code that isn't a function start → kept raw and reported as "mid-code"
    (packed numbers like `0x80018000`, and the text module's internal jump tables);
  - otherwise in PS1 RAM → `psx_ram + offset`; scratchpad → `psx_scratchpad + offset`.
- Zero runs of 4 KB or more aren't stored, since `psx_ram` starts zeroed. Images: main 47 KB, ovl_0b 42 KB,
  the rest smaller.
- Nothing under `build/` is committed. The repo commits only tools and configuration.

### 3.5 PS1 RAM: one arena, by address
**All PS1 data lives at its PS1 address in `psx_ram[]`** (2 MB, `psx_mem.h`). Code is native. This one rule
removes the aliasing problem: on PS1, main's `.data` (0x800665E0–0x801EE000) contains the overlay slots and the
heap, so `g_btlUi` (0x80104F88) and the literal `0x80104000` are the same memory.
- `PSX_ADDR(a)` = `psx_ram + (a & 0x1FFFFF)`, as an integer, as the literal was.
- `build/gen/psx_syms.ld` places every data symbol the decomp knows (symbol files and splat's labels):
  `PROVIDE(g_gameState = psx_ram + 0x6794C)`. Names and literals therefore always agree. Data names need no
  namespace, since the name is the address.
- C-defined globals (a few `const` strings in ovl_0c, one 4-byte variable in ovl_0d) are ordinary ARM symbols.
  Overlay ones get the overlay prefix.
- The ~15 uses of `0x80000000` are mostly flag bits. `ovl_0a.c:6423` `p > 0x80000000` is a pointer test that
  breaks with `0x02xxxxxx` pointers: R4.

### 3.6 Link (implemented in M0)
`make` → `build/ff9ds.nds` (DSi-mode specs, the SDK Maxmod ARM7 core). The link has two passes: the first lists
what is still undefined, and `tools/gen_stubs.py` turns each into a stub that stops with its name
(`build/gen/stubs.txt` is the to-do list). On 2026-10-11 that was 280 stubs: 125 PsyQ functions (`port/psx`),
136 library/BIOS/hand-written asm routines (including the 63-function text module), and 19 game functions
still in asm. The ARM9 binary is 1.04 MB of Thumb code for all 20 binaries plus the port, and `psx_ram` is 2 MB
of bss.

## 4. PS1 compatibility layer (`port/psx/`)

| module | replaces | approach |
|---|---|---|
| `gte.c` | GTE macros and libgte (`RotTransPers`, `ratan2`, `SquareRoot0`, ...) | a software GTE with one global register file, so split macro sequences (`_b`, `_nn`, `gte_ldv0` … `gte_rtps` … `gte_stsxy`) keep their state. Integer-exact to psx-spx; the repo is GPL (D4), so a proven emulator GTE core can be adapted with attribution. `RTPS`/`RTPT`/`NCLIP`/`AVSZ`/`MVMVA` in ARM asm in ITCM, using the hardware divider |
| `gpu.c` | libgpu (`DrawOTag`, `PutDrawEnv`, `LoadImage`, `ClearImage`, ...) | the OT walker and the GX translator (§5) plus a 1 MB VRAM shadow |
| `cd.c` | libcd plus the game's CD module (`cd_startRequest`, `cd_onSectorReady`, `CdGetSector`) | sector reads from `ff9ds/discN/FF9.IMG` on SD through ARM7 file I/O, completing asynchronously on the same callbacks |
| `spu.c` | libspu plus the raw SPU registers | an emulated register file and SPU RAM shadow; voice events go to the ARM7 driver (§7) |
| `pad.c` | libpad/BIOS pad | DS keys plus touch-synthesised buttons → a PS1 digital pad report (§ dual-screen) |
| `card.c` | libcard / BIOS `_card_*` | a file-backed 128 KB memory card per slot (`ff9ds/cards/cardN.mcr`); the raw format is compatible with emulators |
| `etc.c` | `VSync`, callbacks, root counters, events | DS VBlank IRQ; root counter 2 (AKAO 240 Hz) → DS timer IRQ |
| `mdec.c` | libpress / `StSetStream` | FMV is not decoded from STR on device. The FMV entry point is overridden to play a transcoded `.fv` file (§6.3) |

A **host build** (`make pc`, 32-bit Linux, SDL2) compiles the same game code and `port/psx/` with a simple
OpenGL backend for the GX translator. It exists to iterate in seconds, debug with gdb/ASan, and run
**lockstep comparisons**: feed the same pad input to PCSX-Redux (via the decomp's `tools/emu.py`) and the host
build, and diff `g_gameState`, GameData and the RNG pointer each frame. That's how a porting bug gets told apart
from a decomp mismatch.

## 5. Graphics

### 5.0 Hybrid renderer (decided 2026-10-17)

The software GPU (`port/psx/softgpu.c`) is exact but about 5x too slow for battles: one Ark scene (one per four
VBlanks, a 66.7 ms PS1 budget) costs about 330 ms, most of it polygon fill. The all-GX translator (5.1) failed on
texture VRAM (Ark's converted regions need 520 KB, overlapping and duplicated) and on its CPU shadow work.
The port therefore splits the work:

- **Polygons drawn into the scene's back buffer go to the 3D engine.** Everything else (sprites, tiles,
  rectangles, lines, fills, and any primitive drawn straight into the displayed buffer, such as the battle UI's
  per-VBlank windows, text and ATB bars) stays on the exact software path into the PS1 VRAM shadow.
- **Ordering.** Each GX polygon's depth is its submission sequence. The CPU also rasterises polygon *coverage*
  (no texturing) into a byte plane over PS1 VRAM. A 2D pixel drawn where an earlier polygon covers it goes to the
  *over* layer, otherwise to the *under* layer, which the 3D layer hides wherever a polygon covers it. A sprite
  sandwiched between two overlapping polygons is the known approximation.
- **Semi-transparent 2D over 3D** blends in the 2D engine (mode 0: 50/50, mode 1: additive, mode 3: B + F/4,
  by layer or OBJ alpha). Mode 2 (subtraction) over 3D is approximated.
- **Textures mirror PS1 VRAM**, with no conversion. A DS texture bank holds a 256x256-halfword cell of PS1 VRAM
  byte for byte. A 4-bit, 8-bit or 15-bit page is then just a texture format, base address and coordinate offset
  over the same bytes (the PS1's nibble and byte orders match the DS's 16- and 256-colour formats). CLUT rows
  mirror into texture palette VRAM the same way. PS1 transparency is CLUT colour 0x0000 and the DS's is palette
  index 0; nearly all of FF9's CLUTs have 0x0000 at index 0 only, so the DS rule matches. Other CLUTs and
  15-bit pages need a converted copy. Dirty rows are re-copied in VBlank.
- **Polygon budget.** The 3D engine takes 2048 polygons and 6144 vertices per frame. Ark's scene is about 2,030
  primitives and 6,800 vertices, so a scene over budget renders in passes over successive VBlanks (a battle scene
  lasts four), with the earlier pass captured and shown behind the later one.
- **Readbacks.** A game read of a framebuffer the 3D engine drew (`StoreImage`, `MoveImage`, a page sampled from
  a framebuffer: battle swirl, blur) is served by display capture into the VRAM shadow.
- **Fidelity.** 3D pixels are rasterised at 256x192 by the DS (no PS1 dithering; modulation above 1.0 and blend
  modes 1-3 within the 3D layer are approximated). 2D pixels stay exact, including the scaler's text coverage.

VRAM: textures need up to four 128 KB cells (A-D; Ark uses four). Texture palettes go in F and G. 2D layers are
OBJ bitmap blocks in E when the textures use A-D (battle), or BG bitmaps in C/D when two texture banks suffice
(field). The implementation is `port/ds/gx_hybrid.c`; `SOFTDISPLAY=1` keeps the all-software reference.

**Implemented so far (`make ds HYBRID=1`, battle only):**
- A scene is recorded as clipped primitives; when the game shows its buffer, textures are planned for the whole
  scene (one region per page and depth, the texels' bounding box; regions that persist keep their place), dirty
  regions and CLUTs are copied in, and the commands go to the geometry FIFO by DMA, ending with the swap.
- The 2D layer is the displayed buffer's pixels other than its scene's clear value (polygons never reach PS1
  VRAM, so nothing else differs), on an 8-bit bitmap BG in E whose palette is built as colours appear (the battle
  UI uses under 200 colours, so it is exact; a full palette falls back to nearest colours and a rebuild). A 2D pixel exactly equal to the clear value shows
  the 3D layer instead; 2D drawn before a polygon still lands on top (no coverage plane yet). Blocks are rebuilt
  when their source cells were drawn and their pixels hash differently.
- Semi-transparent sprites and tiles drawn into a scene buffer are quads of the 3D layer (their U and V are the last
  texels; the right and bottom vertices sample one further), so they blend with it. Opaque ones stay exact on the
  2D layer: as quads they would go under 2D pixels drawn before them (a window's background over its text).
- Texture regions are stable (U and V in 64-texel steps), so a page keeps its place across scenes. Battles use
  about 420 KB of real texture data (seven or more full 8-bit pages), so they need all four banks A-D.
- Semi-transparent polygons, modes 1-3 (B + F, B - F, B + F/4), use per-texel alpha. Textured ones use an A5I3
  copy: each texel's alpha is its brightest channel and its colour is divided by it, so the 3D engine's
  src + dst(1 - a) equals the PS1's src + dst wherever the source is dim, and only bright texels hide what is behind
  them. Eight colours per texture (farthest-point over its CLUT, or over a sample of a 15-bit page's texels) sit in
  a reserved palette slot; texels without the semi-transparency bit are opaque. Untextured ones (glows) sample a
  fixed 32x8 ramp whose alpha rises along U, each vertex's U being its brightness: the per-pixel alpha the DS lacks
  per vertex. The vertex brightness, and mode 3's quarter, go to the polygon alpha; the vertex colours keep the hue.
  Mode 2 is drawn black at twice the source brightness: dst(1 - 2s) equals the PS1's clamped dst - s at mid grey
  and is black where it is. Mode 0 is polygon alpha 16 (exact except for texels without the bit).
- A translucent polygon sharing an edge with one of the last 16 from the other side of it takes that one's ID, so a
  mesh's seams aren't blended twice; separate overlapping layers, and a closed mesh's back faces, still blend.
- A scene over the hardware budget (Ark and Taharka run 4-6% over the 6144 vertices) drops small primitives from
  the start of the drawing order, the far scene; size alone would take the nearest character, whose model is many
  tiny triangles drawn last. Strips don't help: consecutive primitives almost never share an edge with identical
  attributes, and their depth values (the drawing sequence) differ anyway.
- The UI's redraw skipping draws each entry over the dirty area clipped to it (drawn whole, an unchanged window
  background covered a skipped glyph outside it).
- Known differences: the brightest additive layers saturate less than the PS1's sums; mode 2 darkens evenly where
  the PS1 shifts hue; semi-transparent UI (the selected row's bar) blends with the buffer's clear colour, not the 3D
  layer, since the DS blends whole layers and no OBJ VRAM is left for a semi-transparent sprite layer.
- To do: field layout (BG bitmaps, coverage-based under/over), readbacks of GX-drawn buffers.

### 5.1 Earlier baseline: translate the whole OT (M2, superseded by 5.0)
At `DrawOTag`, walk the ordering table in PS1 order: last slot first, and within a slot the most recently added
first. Each packet translates to DS GX commands:
- **Coordinates** are already screen-space after the GTE. An orthographic projection maps the 320-wide frame
  to 256 (×0.8, uniform). 224-line modes letterbox to 179 lines plus 6-line bars. 240-line modes come out at
  exactly 192.
- **Depth**: each emitted polygon gets `z = slot × K + seq` in draw order and a depth test of less-or-equal.
  The result matches painter's order. Translucent polygons use manual sort (`GL_TRANS_MANUALSORT`), so they draw
  in submission order.
- **Textures**: the texture page (tpage) with its colour lookup table (CLUT), keyed by (page, depth, CLUT), maps
  to a DS 256×256 texture: 4bpp → 16-colour, 8bpp → 256-colour, 15bpp → direct. The bit order and the BGR555
  colours are identical. CLUTs go to palette VRAM. A colour value of 0x0000 is transparent (DS "colour 0
  transparent"). Conversion reads from the VRAM shadow, which `LoadImage`/`MoveImage`/`ClearImage` keep dirty-tracked
  per 64×64 block. Uploads happen in VBlank and are budgeted per frame, with an LRU over texture slots.
- **Texture modulation**: PS1 0x80 = ×1.0 and up to ×2. DS tops out at ×1.0. Values above 1.0 are approximated
  with highlight shading via the toon table, or clamped (fidelity item F1).
- Lines become degenerate triangles; `TILE`/`SPRT` become quads; `DR_MODE`/`DR_TPAGE`/`DR_TWIN` update state.
- **Offscreen passes**: a `DR_AREA`/`DR_OFFSET` outside the display buffer, or a `MoveImage` from a framebuffer
  (screen capture for the battle swirl, blur, fades), is serviced by **display capture** into a VRAM bank, which is
  then used as a 256×256 direct texture. Every site still has to be classified (R7).

### 5.2 Blending
The PS1 has four semi-transparency modes. The DS 3D engine only alpha-blends.

| PS1 mode | use | DS |
|---|---|---|
| 0: B/2 + F/2 | windows, shadows | polygon alpha 16/31, exact |
| 1: B + F | glows, magic, light shafts | per-texel alpha from brightness (5.0, implemented): exact for dim texels. Not yet: a 2-pass composite (opaque + alpha geometry captured, then additive polygons on black blended with EVA=EVB=16) for the bright ones |
| 2: B − F | darkening effects | approximated as alpha-blended black at the source luminance (F2) |
| 3: B + F/4 | subtle glow | as mode 1 with a quarter of the polygon alpha |

### 5.3 Native paths (M6, only where profiling says so)
1. **Field backgrounds** (`o0a func_800BE158` adds a layer's 16×16 tiles to the OT). A field layer can be
   hundreds of `SPRT_16`s, which would eat the 2048-polygon budget. Native path:
   - At field load, composite each BG layer from the VRAM shadow into a full layer image.
   - Far layers become 2D BGs on the main engine, behind the 3D layer (8bpp tiled with extended palettes, or a
     16bpp bitmap), scrolled in hardware.
   - Layers with depth (characters walk behind them) become a few large textured 3D quads at the layer's
     depth, so the depth buffer gives per-pixel occlusion, which is better than the PS1's OT bucketing.
   - Animated layers (`func_800BB090`, VRAM animation) re-upload only their changed blocks.
   - Optionally the extractor pre-scales BGs by 0.8 with a good filter, so they aren't sampled nearest-neighbour.
2. **Models**: field and battle skinned models (bone matrices on the scratchpad, `o0a` around ovl_0a.c:5430/6144,
   the `o0c` equivalent) get converted at load into DS display lists, so bones run on the DS matrix stack and the
   transform runs in hardware. The ARM9 then only does game logic.
3. **Text and windows**: rendered as 2D tiles (see `dual-screen.md`), not as GPU sprites.

**Battle models and background native (2026-10-25, first step of item 2).** `btl_drawMesh` and `btlbg_drawRecord`
are wrapped (config/renames.txt renames the game's versions in their own objects to `*_packets`;
port/game/mesh_native.c): while the hybrid renderer runs, a model mesh is kept as its source lists, a copy of its
projected vertices (`gte_rtpt_batch`) and the texture words it uses; a background record as its projected vertices
(written to the scratchpad as btlbg_project does: a record may index an earlier record's) and each polygon's OT entry
by the record's rules. Each polygon is classified once (the PS1's culling, no area, off the buffer, its texture use,
its size tier, translucent or not: a byte). They are kept with the buffer the frame's OT is drawn into
(`drawEnv[unk8]`: the game builds frame N while its target buffer is still shown, btl_endFrame) and matched to the
scene by `psx_ot_generation`. No packets are written, walked, decoded, recorded or planned one by one.

Order: the walk notes the main OT entry it is in (gpu.c `psx_ot_slot`; the main OT is the one ClearOTagR clears with
0x1000+ entries) and the renderer keeps, per entry, how many primitives were recorded when the walk got there. At
submit the native polygons are sorted by the entry their packet would have had (quads: the depth sum >> 6; triangles:
AVSZ3 / 4; less the mesh's bias; records: theirs), far to near and the most recently linked first within an entry,
and merged among the recorded primitives by those counts; everything goes out at increasing depth, so the 3D engine
draws the PS1's order. (A first version gave model vertices their own depths for a per-pixel sort: a monster's wing
tip went behind the wall it is drawn over on the PS1, and which pixels flipped depended on the scene's other
primitives.) The polygon budget counts them after culling and drops the smallest first across both kinds (by size
tier; within the last tier, as many native ones as it takes). The renderer's per-primitive tables (10 KB) moved to
the mode overlays' dead code (battle: after the per-entry tables; field: after the buffers, OVL0A_FIELD_PRIMS 1450 ->
1330 when both are in ovl_0a); battle buffers keep 1200 primitives, the rest of each holding its native polygons.

Over most of a battle (PROFWIN=1024): d2_b0098 36.3 -> 30.8 ms, d1_b0186 33.6 -> 29.3 (13-15%); drops 43000 ->
3500 a run (polygons off the buffer no longer count). The frames match the packet path's (with its faults: d1_b0186's
spell shows a model the host doesn't). Effect models (`btl_drawMeshOffset`) keep their packets. Next: the hardware
transform with display lists (item 2 proper).

### 5.4 Polygon budget
DS: 2048 polygons per frame. PS1 FF9 counts per scene aren't known yet. R9 asks for primitive counts per frame
from the emulator (field, a dense field, the world map, a normal battle, a summon, Tetra Master). If a scene goes
over, the order of fixes is: native field BG (5.3.1), merging sprite runs, then dropping or simplifying the
effects that exceed the budget.

## 6. Assets

### 6.1 Extraction (`make extract`, PC side)
Inputs: the four disc images, in raw 2352-byte `.img`/`.bin` with `.cue`/`.ccd`, or `.chd` via chdman. The
extractor checks the sha1 of `SLUS_012.51` and each `FF9.IMG`.
- Build-time outputs (`build/`): splat output → generated data (§3.4), pristine overlay `.data` images, the font
  atlas (6.4).
- Runtime folder `ff9ds/` (copied to the SD card, ~1.2 GB):
  - `discN/FF9.IMG`, read by sector exactly as the PS1 does. Later, an optional dedup pack across discs.
  - `fmv/*.fv` (6.3)
  - `cards/`
  - Phase 6: `bg/` (pre-scaled field BGs, once the BG format is identified).

### 6.2 Disc layout facts (disc 1)
`FF9.IMG` (332 MB); `OPENING/FMVD001.STR`; `SEQ01..03/FMV*.STR`; and `MBG*.MBG` (movie backgrounds: FMV played
*behind* 3D characters during field scenes). Discs 2–4 have the same layout with their own `SEQ` directories.

### 6.3 FMV and movie backgrounds
- STR (MDEC video + XA audio) goes to FFmpeg (it has the `psxstr` demuxer and the `mdec` decoder), then to the
  **FastVideoDS** encoder (`.fv`; the player decodes ~30 fps on DS and 60 on DSi, using the 3D engine for motion
  compensation and the ARM7 for SD reads and audio).
- The encoder needs an AVX2 CPU. The output is scaled from 320×(224|240) to 256×(179|192).
- Full-screen FMV plays on the top screen. The bottom screen shows chapter art or stays black (D3).
- **MBG**: frames decode into a main-engine 16bpp bitmap BG. The 3D characters render on BG0 above it, using
  the field camera. MBG sync with field scripts is R10.

### 6.4 Text
The game's font glyphs are extracted from the user's data and converted into a 4bpp tile font at two sizes:
1:1, and a ×0.8 anti-aliased version for windows on the top screen. Text stays in the game's encoding; the
renderer reproduces the opcode interpreter's layout (R15).

## 7. Audio

### 7.0 Original-DS sound path (decided 2026-10-17)

The exact software SPU (24 voices, Gaussian interpolation, reverb) costs about 16 ms per 60 Hz frame on the DSi's
134 MHz ARM9, so about 32 ms on the DS's 67 MHz: twice a frame by itself, and the ARM7 (33 MHz) is far slower.
The port targets the original DS, so the DS's 16 hardware channels do the mixing:

- The SPU stays emulated on the ARM9 for everything the game observes: registers, key on/off, ADPCM block
  stepping (loop and end flags, ENDX, IRQ addresses) and ADSR levels, which AKAO reads back to free voices.
  Only interpolation and mixing go away.
- Each voice's decoded ADPCM samples, in the order the voice plays them (loops included), go into a per-voice
  ring buffer that a DS channel plays at the voice's pitch. A key-on starts the channel one frame later, once
  that frame's samples are written.
- Each frame, a voice's ADSR level, its volumes and the main volume set the channel's volume and pan, and its
  pitch sets the frequency. Envelopes thus move in 60 Hz steps.
- At most 16 voices sound at once: the loudest get channels. Pitch modulation is per frame. Noise voices use the
  PSG noise channels (14, 15). Reverb is an echo through the capture units (decided 2026-10-24): while the game has
  reverb on with some voice sending to it, capture unit 0 records the left mix into a ring that channel 1 plays back
  a delay later (unit 1, the right mix, channel 3), so each echo returns quieter; the level follows the reverb output
  volume (at most 3/4) and the delay the reverb work area's size, up to 0.28 s at 11 kHz. Per-voice sends and the
  reverb's filters aren't modelled, and voices then share 12-14 channels.
- The exact mixer stays as the host reference and the DSi diagnostic build (`HWSOUND=0`).

This gives up bit-exact PS1 audio: no Gaussian interpolation (the DS resamples by nearest sample), envelope
detail below 1/60 s, voices beyond 16, and exact reverb.

- **AKAO stays on the ARM9**, unchanged C, driven by a 240 Hz timer IRQ as on PS1 (root counter 2). It writes
  the emulated SPU registers.
- **SPU transfers** (`SpuWrite`/DMA) land in a 512 KB SPU RAM shadow. Decode blocks as voices fetch them;
  predictor history persists across blocks and loops, and RAM writes must remain observable. Sample caching
  must preserve those semantics rather than infer independent samples from block flags.
- **Shared software SPU target**: all 24 voices at the PSX's 44.1 kHz sample clock, including Gaussian
  interpolation, ADSR, volume sweeps, pitch modulation, noise, and the original reverb processing. Voice stealing,
  1 kHz envelope approximations, and reduced-rate reverb do not satisfy the fidelity objective.
- **DS output target**: stream the mixed stereo PCM through DS sound hardware. The ARM9 now queues
  full-rate stereo PCM for Maxmod, whose ARM7 core drives two hardware channels; assigning voices to the 16 native channels must not discard PSX voices or change
  the mixing arithmetic. First establish deterministic host reference comparisons, then verify DS output and
  real-time timing. Keep AKAO register writes and key events ordered relative to generated samples.
- **Current implementation**: host `--audio NEW.wav` renders game audio with reverb at 44.1 kHz through the AKAO
  register boundary and interleaved virtual timer clock. All 24 voices, Gaussian interpolation, ADSR/volume
  sweeps, pitch modulation, shared noise, reverb, main-volume mixing and basic capture RAM are implemented/tested;
  see `audio-validation.md`. Host `--spu-trace` captures game inputs for replay against
  pinned voice/reverb references. IRQ/DMA/capture timing fidelity, CD/movie inputs, physical-console
  comparison and real-time DS playback without starvation remain unfinished. DS audio is enabled by
  default (`AUDIO=0` is a diagnostic baseline); default host mixing stays disabled until a sink
  is selected. DS source PCM matches a captured title interval on the host, but gameplay still
  produces audible gaps because rendering/mixing cannot keep pace. See `audio-validation.md`.
- **ARM9 DSP placement**: `SPUARM=1 SPUITCM=1` compiles the seven shared sound-core
  modules as ARM instructions in ITCM. The normal link currently uses 17,984 of
  32,768 ITCM bytes including raster/scaler and SDK code/data. Diagnostic switches
  permit Thumb/main-RAM comparisons without changing sound arithmetic.
- FMV audio must preserve its original stream timing; the planned FastVideoDS integration is not implemented.

## 8. Memory budget

Measured 2026-10-16 with default `make ds` (software display and audio enabled):

| region | bytes | accounting |
|---|---:|---|
| ELF text / read-only data, including ITCM | 1,196,004 | all native overlays and generated relocation images remain resident |
| ELF initialized writable data | 456 | |
| ELF zero-initialized data | 2,233,908 | includes the 2,097,152-byte PSX arena and audio queue |
| **ELF static total** | **3,430,368 (3.27 MiB)** | `size build/ds/ff9ds.elf`; includes regions outside main RAM, not a heap high-water measurement |
| VRAM shadow allocated at boot | 1,048,576 | `psx_mem_init()` |
| SPU RAM allocated at boot | 524,288 | `psx_mem_init()` |
| **Static + these two allocations** | **5,003,232 (4.77 MiB)** | excludes other heap allocations, filesystem buffers, stacks and loading peaks |

This is a footprint tally, not an exact main-RAM occupancy measurement: ITCM is
separate RAM, the loader needs initialization bytes, and alignment/TLS/reserved
regions require linker-aware accounting. It already demonstrates that the
current implementation cannot fit in 4 MiB. There is no implemented 2 MiB PCM
sample cache or real movie decoder buffer to count yet. See `ds-mode.md` for
reproduction commands, the removed inactive GX buffers, and the required design work.

**VRAM banks** (planned layout):

The current M2 GX renderer uses A/B/C/D for textures (512 KiB), E for texture palettes (64 KiB),
and H for the debug console (4bpp font and tile map). The specialised field backgrounds
and capture effects below are not implemented yet; adding them will reduce texture capacity.

| bank | use |
|---|---|
| A, B | textures (256 KB) |
| C | textures; or the capture target for the 2-pass and offscreen effects (dynamic) |
| D | main-engine BG (field layers, MBG bitmap) |
| E | texture palettes (64 KB) |
| F/G | main OBJ and extended BG palettes |
| H/I | sub engine (bottom screen UI, 48 KB) |

**Original DS (4 MiB)** requires native code/data overlays and a measured graphics,
audio and streaming working set. Removing the VRAM shadow, shrinking the PSX arena
or converting sound formats are not assumed safe: each changes semantics that
still need to be preserved. This is an architecture constraint now, rather than
a separate post-M7 effort; see `ds-mode.md`.

## 9. Repository layout (planned)

```
decomp/            git submodule: ff9-decomp, pinned
config/            overrides.txt, xrefs.txt, addr_remap.txt, ovl namespaces (generated + hand annotations)
port/include/      port_prelude.h, psx_types.h
port/psx/          gte, gpu (OT walker + GX), vram, cd, spu, pad, card, etc
port/ds/           platform: video setup, texture cache, capture, file I/O, timing, touch
port/ui/           bottom-screen UI per game mode
port/game/         overrides of game functions (loader, BG renderer, UI hooks)
port/pc/           host build backend (SDL2/OpenGL)
arm7/              SPU emulation + ADSR + reverb + FMV audio glue
tools/             extract, gen_data, find_xrefs, layoutcheck, fmv transcode, font conversion
build/  ff9ds/     generated; never committed
```

Licence: port code GPL (D4); tools CC0 like the decomp's.

## Field 3D option (FIELDGX=1, 2026-10-24)

`FIELDGX=1` runs the hybrid renderer in fields. Its scene buffers hold 1880 primitives each: the first in ovl_0a's
dead MIPS code range (`ovl0a_layout.h`), the second in native slot 0 after the field overlay's ARM code (the slot is
sized for battle's; about 1680 in DS mode, where battle is built `-Os`). The GX command chunks are in VRAM bank I (in
battle too). A field frame is 3D or software:
- 3D: every primitive drawn into the back buffer is a 3D quad or polygon in scene order (rows of background sprites
  merged into one quad), with all four texture banks. The UI pages' primitives (text, window parts, texture windows
  included) are replayed in software on the 2D layer above.
- software: when its textures don't fit, or the scene can't be drawn by the 3D engine (more than its 2048 polygons
  or 6144 vertices, which is 1536 separate quads; a texture window off the UI pages; mask bits; a readback of the
  frame), the scene is replayed and drawn by the software renderer, scaled into bank A (a 16-bit BG behind a
  transparent 3D layer). For the next 60 frames the GPU goes straight to the software renderer (the hybrid's
  per-primitive bookkeeping costs 2 us a primitive). Those frames are pixel-identical to the software path.
Textures: a region per page (its 64-texel-stepped bounding box, per CLUT for additive copies). A field whose regions
don't fit switches its sprites to **block runs** until the mode ends: 16x16-texel blocks, copied as runs of a page
row's consecutive blocks the scene samples into 256x16 rows, so only what is drawn is held (field 931: 576 KB of
regions, 334 KB of runs and polygon regions). A sprite's texels are consecutive blocks of one row, so a merged row
of sprites stays one quad. Fields that fit keep regions: runs cost a texture switch per sprite, 2-3%.
Lines (Burmecia's rain, field 767) are 3D too: a triangle with two equal vertices, which the DS draws as a one-pixel
line, clipped to the drawing area; on the 2D layer they had blended with the buffer's key and covered the scene.
Additive primitives (PS1 modes 1 and 3) covering up to a sixth of the screen are exact: drawn in software onto black
in bank A and added over the 3D layer by the 2D compositor (BLDCNT, both coefficients 16, saturating as the PS1),
in the rows they cover; textures for the rest then use banks B-D (else the approximation below). Field 2910's window
light turned white as on the PS1 (it was blue); 1425's tunnel exit and 1203's lamps match. The layer lies over the
whole 3D layer, so something drawn after a glow would be brightened too. Larger additive areas (2162's lamps: 27 ->
40 ms on DSi) keep the approximation.
Subtractive and additive blends drawn by the 3D engine are approximated as in battle (a subtractive shadow over a
character, the commonest case, is in nearly every scene; additive mist over a dark tile can show as a lighter
square). Scenes hold up to 2774 primitives (median 1012, 90% 1681, 10 of 563 fields over the 3D engine's 2048:
host census, 2500 frames of random walking with `--render`).

28 DS sample fields at frame 1500 (DSi, PROFILE=1), software path -> FIELDGX: 26 in 3D, faster by up to 37% (1102
43.4 -> 27.5 ms, 453 54.3 -> 35.5, 306 31.8 -> 21.2, 2055 39.8 -> 26.1, 2110 57.7 -> 45.8, 955 54.3 -> 44.6, 1414
44.8 -> 39.7, 2162 42.1 -> 36.5, 1508 47.3 -> 42.3), and looking the same apart from the 3D engine's sampling at
the 0.8 scale and the additive approximation. 931 and 1812 are over the 3D engine's limits at that frame (2300 and
2200 primitives): software there, as fast. Before block runs and the larger buffers, 8 of the 28 were software.
Frame times without timers (PROFILE=2, real ms per PS1 VBlank, the port's virtual frame: 16.7 is the PS1's speed, since
the game paces itself in VBlanks (fields `VSync(2)`, 30 fps; battles `vsyncMode` 4, 15 fps); 33.3 is half), after the profiling
pass of 2026-10-24 (PROFILE=3 sampling profiler: no memset per GP0 command, the 2D layer skipping cells nothing drew
in, GP0 vertices decoded in place, GX commands written in main RAM, gp0.c and gx_hybrid.c as ARM):

| field | software | FIELDGX |
|---|---|---|
| d1_f0158 | 16.7151 | 16.7151 |
| d1_f0110 | 22.9548 | 18.0186 |
| d1_f0615 | 17.5836 | 16.7151 |
| d1_f0453 | 50.1074 | 26.1172 |
| d1_f0306 | 29.1453 | 16.7055 |
| d1_f0205 | 22.3423 | 17.7571 |
| d1_f0162 | 29.0435 | 16.7151 |
| d2_f0931 | 53.9236 | 43.3793 |
| d2_f1425 | 20.3372 | 17.9038 |
| d2_f1203 | 20.3511 | 16.7151 |
| d2_f1414 | 37.8383 | 29.2742 |
| d2_f1102 | 40.3863 | 21.4316 |
| d2_f0955 | 47.3384 | 34.2149 |
| d2_f1057 | 27.5401 | 24.1081 |
| d3_f2714 | 19.4641 | 17.2131 |
| d3_f1551 | 16.8544 | 16.7151 |
| d3_f1811 | 16.7151 | 16.7151 |
| d3_f1508 | 37.6124 | 31.6175 |
| d3_f1812 | 58.503 | 61.2536 |
| d3_f1606 | 24.3836 | 16.7151 |
| d3_f2055 | 35.6496 | 20.3223 |
| d4_f1706 | 38.5336 | 27.9818 |
| d4_f2934 | 19.133 | 16.7151 |
| d4_f1953 | 16.7148 | 16.7151 |
| d4_f1911 | 24.1168 | 18.5251 |
| d4_f0351 | 28.8219 | 26.5027 |
| d4_f2110 | 47.6403 | 33.8488 |
| d4_f2162 | 36.1143 | 27.2725 |

9 of 28 at the PS1's speed (16.7 ms), 13 within 8% of it, 24 of 28 at or under half of it (33.3 ms; an earlier reading of these
numbers as per 30 fps game frame took 33 ms for the PS1's speed: it is 16.7). 931 and 1812 are over the 3D engine's limits for many scenes (1812 is 2.5 ms
slower than the software path: its software frames go through bank A, `scale_back`); 955 and 2110 are within 1 ms.

Tried for scenes over the limits: split frames, the back part (the primitives before the last ones that fit, or a
full buffer's older half) drawn in software into bank A and the rest in 3D over it. They look right (931 at frame
600, 1812) but aren't faster: 931 58.9 -> 58.5 ms, 1812 71.0 -> 79.4 (the back's software drawing, recording every
primitive and scaling both layers cost what the 3D part saves). Those scenes stay software; drawing all of them in
3D needs two 3D passes (display capture of the back half, shown behind the front half).
Tried first: a software background layer for the sprites before the first
polygon, to keep their textures out of the 3D engine; re-rendering it each frame (the game writes VRAM every frame,
and which sprites move there changes as characters move) cost more than it saved.

## Scenes over the 3D engine's budget: options (2026-10-25)

Battle scenes can exceed the 3D engine's 2048 polygons and 6144 vertices (Ozma's at frame 1500: 1768 primitives, a
few over the vertex RAM), and `plan_drops` dropped the excess: a fidelity loss on DSi and DS alike. Fields over
budget (931, 1812) go software instead, exact but slow. Options considered:
- Two 3D passes with display capture: pass 1 (the back) captured to a VRAM bank and shown behind pass 2 (BG3), the
  previous complete frame shown from a second bank (VRAM display mode) while pass 1 renders. Needs two of A-D, which
  halves texture memory while it runs (Ozma holds 354 KB of textures); per-pass texture swaps would have to fit in
  VBlank. Feasible on DSi with the display FIFO from main RAM instead of the second bank; tight on a DS.
- Split frames (tried for fields, see above): the back part drawn exactly in software and scaled into bank A behind
  a transparent 3D layer. Correct and as fast as full software in fields. In battles the software-drawn UI shares the
  PS1 buffer, so the back part must land before it in drawing order yet stay off the 2D layer: that needs either the
  UI recorded and replayed (as fields do) or a per-pixel "background" mark that later drawing clears (the existing
  mark plane is set-only and serves additive glows). Fits the DS's memory; the most promising for the original DS.
- Smarter dropping (only primitives invisible at the 0.8 scale): lossless only for sub-pixel ones.

Tried: split frames for battles, recording a battle's 2D-layer primitives (opaque sprites, texture windows, lines)
for replay when the scene is sent, as a field's UI is. 4 bosses matched; 8 more showed missing or damaged windows and
text (Garuda, Plant Brain, Nova Dragon) and slowdowns up to 60% (Antlion 59 -> 91 ms): reverted. Scenes over the
budget are rarer than first read (Ozma: 42 primitives over), so dropping stays until the battle UI's replay is
understood.

## Additive light in battles (2026-10-25)

Battle spell effects (Maliris, Tiamat, Necron in the boss sweep) are many overlapping additive sprites that saturate
toward white on the PS1. The DS's 3D engine only alpha-blends, and its 2D compositor adds two layers at most:
- a capture pass (the additive primitives rendered by the 3D engine onto black, captured, added over the scene by the
  compositor) adds the effect to the scene exactly, but the effect's own layers still stack by alpha, so explosion
  cores stay coloured (and texture memory halves while it runs);
- the fields' software layer (exact, stacking included) costs in proportion to area, and battle effects cover much of
  the screen: far too slow on a DS, borderline on a DSi;
- a whiter approximation (biasing bright texels' colours toward white) helps stacked cores but over-whitens single
  glows over dark backgrounds.
Exact battle effects therefore trade speed (software) or exactness (approximations); a decision for the user.

### Additive light whiter (2026-10-25, the user's choice)

The 3D engine draws additive light as dst x (1 - a) + hue x a, the PS1 as dst + source; over a background d the two
agree with d added to the hue (per channel, clamped), and on the PS1 that background is the light already added, which
is what turns stacked layers white. `gx_hybrid.c` adds a base of 4/31 to the additive palettes and vertex colours, and
to each additive primitive's vertex colours the light earlier additive primitives of the scene put under it (per
channel, per 16-pixel cell whose centre it covers, the texture's brightest colour times the vertex hue), capped at
8/31 and only for primitives within 24 cells: a single amount per primitive shows a large primitive's shape where
the light under it varies (Necron's flash at 12/31 and 48 cells). Against the host's frames: Tiamat's beam and Lani's
sphere much closer to the PS1's white, Maliris's streaks whiter (its fireball still orange: an orange texture under
white vertices, which only the shared palette could whiten), the other bosses and the fields unchanged. Costs 1.1 KB
of code and 1 KB of tables; no measurable time.
