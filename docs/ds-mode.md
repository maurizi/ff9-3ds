# Original DS feasibility and architecture

The user's preferred target is original DS mode, revised 2026-10-16. DSi is the
working reference build while the RAM and CPU work is developed. Original DS
support is not yet implemented or demonstrated. Do not trade away PSX effects,
voices, reverb or game timing merely to claim that the game fits.

## Measured starting point

BlocksDS documents [4 MiB main RAM on DS and 16 MiB on DSi](https://blocksds.skylyrac.net/docs/internal/build_process/).
The current platform selects the faster ARM9 clock only when `isDSiMode()` is true;
original DS mode has roughly half the ARM9 clock budget. The current build always
uses `dsi_arm9.specs`. `make ds-r4` only patches the DLDI driver into that same
ROM: it does not establish a DS memory layout or working DS support.

Default build footprint, 2026-10-16:

| measurement | before excluding inactive GX | after |
|---|---:|---:|
| ELF text / read-only data | 1,220,108 | 1,196,004 |
| ELF initialized data | 468 | 456 |
| ELF BSS | 3,996,664 | 2,233,908 |
| ELF static total | 5,217,240 | 3,430,368 |
| static + allocated VRAM/SPU shadows | 6,790,104 | 5,003,232 |

All figures are bytes. Savings: 1,786,872 bytes (1.70 MiB). Strong
`psx_vram_touch` and `psx_vram_read` hooks had retained GX texture caches,
conversion buffers and the 983,040-byte primitive buffer even with software
display selected. The software build now uses `gpu.c`'s weak no-op hooks; GX
builds still compile their full backend. This is not a reduction in the active
renderer quality or PSX memory capacity.

The static tally includes ITCM as well as main RAM. It is not a precise main-RAM
high-water measurement: TLS, alignment, initialization load images, stack and
SDK reservations also matter. The 4.77 MiB tally still exceeds DS capacity before
other heap users or future real movie decoding. The 17,984-byte ITCM section
does not account for the gap.

Reproduce after `tools/docker.sh make -j16 ds pc`:

```sh
size build/ds/ff9ds.elf
readelf -SW build/ds/ff9ds.elf
readelf -lW build/ds/ff9ds.elf
nm -S --size-sort build/ds/ff9ds.elf | tail -30
```

The map is `build/ds/ff9ds.map`. For a shipping budget, additionally measure heap
and stack high-water marks across loading transitions, battle effects and movies;
do not infer free runtime RAM from the ROM file size.

## Progress (2026-10-17)

* Overlay data images no longer stay resident: an overlay's bytes reach `psx_ram` from the disc as on the PS1,
  and `psx_overlay_patch` converts only its pointer words from a generated relocation list (`tools/gen_link.py`:
  offset and kind; code pointers carry the native address). ELF text+rodata 1,221,300 -> 850,776 bytes. PS1 RAM
  after field 259 (700 frames) and Ark (2000) is identical to the image build; 18/18 scenarios pass. Main's
  47 KB image is still linked in.
* Main's MIPS code range in `psx_ram` (0x800127A0-0x800665E0, ~335 KB) is never loaded by the port and stayed
  zero through field, battle and New Game runs; `psx_spare_alloc` gives it to port buffers (the hybrid's
  primitives, glow bit plane and colour map). The hybrid's GX list goes to the FIFO in two 16 KB halves.
* Totals now (HYBRID=1): about 0.85 MB code and read-only data, 2.4 MB bss (2 MB `psx_ram`, of which 335 KB now
  holds port buffers), 1.5 MB VRAM/SPU shadows: about 4.8 MB against 4 MB, before stacks and SDK heap.
  The remaining gap needs native code overlays (field/battle/world code is mutually exclusive: about 250 KB) and
  a smaller working set for the VRAM shadow or for `psx_ram` regions holding the resident overlay's MIPS code.

## The budget in one line (2026-10-17)

The PS1's own state is 3.5 MB on its own: `psx_ram` 2 MB, the VRAM shadow 1 MB (`psx_vram`) and SPU RAM 512 KB
(`psx_spu_ram`). On a 4 MB DS that leaves about 512 KB for the ARM9 code (744 KB now: 330 KB main and port, 450 KB
overlays of which at most about 180 KB are needed at once), the port's buffers (bss besides `psx_ram`: about
650 KB, the largest being the hybrid's UI list 108 KB, the HWSOUND voice rings 192 KB and the soft display's
96 KB frame), libnds and the stacks. Holes in `psx_ram` that the port never uses (main's MIPS code, 335 KB, already
holding port buffers; the resident overlays' MIPS code) are not contiguous enough for the 512 KB SPU RAM or any
large part of the VRAM shadow.

So exact emulation of the PS1's memories does not fit a plain 4 MB DS even with native code overlays and every
port buffer trimmed. Options, to be decided:

* DS and DS Lite with the Slot-2 Memory Expansion Pak (8 MB, slower 16-bit bus): the VRAM and SPU shadows move
  there. The DSi has no Slot-2, so it would keep DSi mode (16 MB), which already works.
* A plain 4 MB DS with a VRAM working set (framebuffers and recently used texture pages, the rest refetched from
  the disc where the game loaded it from) and SPU RAM in DS VRAM banks a mode leaves free: exactness would depend
  on the game not reading back what was evicted, which needs proving per scene.
* DSi mode (16 MB) only, as now.



* **Disc assets already stream.** The CD replacement reads sectors from disc
  files into the existing PSX arena. We are not retaining whole fields, discs or
  the whole game's sound collection in memory. More streaming helps only when
  it replaces a specific resident copy or bounds an existing working set.
* **Native code does not overlay yet.** All 19 translated overlays are linked
  permanently into the ARM9 executable. The current overlay loader copies only
  relocated data into the original PSX slots. Build native ARM overlay files and
  a resident core with stable entry veneers; load at game mode/overlay boundaries.
  Establish the simultaneously resident set from the three existing PSX slots,
  callbacks, menus, battle effects and nested calls. Do not evict code with live
  return addresses or function pointers. Arbitrary demand paging is unavailable.
* **Generated data also stays resident twice when active.** Relocated image
  bytes are linked read-only, then copied into `psx_ram`. Store overlay images
  and relocation records in runtime files instead. Apply relocations against
  the selected native overlay addresses and resident veneers when loading.
  Cover both explicit overlay loads and sector-by-sector `psx_overlay_patch`.
* **The PSX arena is a real 2 MiB compatibility contract.** Original offsets,
  heaps, script pointers, raw overlay bytes and low-RAM reads rely on it. Keep it
  intact initially. Reusing regions containing inactive MIPS code may eventually
  help, but requires a lifetime/read-reference audit; it is not safe to shrink
  the array or overwrite those bytes globally.
* **Graphics need an explicit working-set design.** The current software
  renderer uses a full 1 MiB VRAM shadow. DS VRAM is separate from main RAM, but
  occupied display/texture banks cannot simultaneously be counted as free
  backing storage. A smaller or bank-backed representation must preserve
  texture/framebuffer feedback, `MoveImage`, `StoreImage`, mask bits, CLUTs,
  blending and offscreen drawing. Native DS-resolution rendering could reduce
  raster work, but does not by itself remove these storage requirements. Keep
  the software implementation as the comparison reference while testing GX or
  tiled alternatives against PSX captures.
* **Audio is not an unbounded decoded cache.** The current 512 KiB SPU RAM is
  original sound memory, with on-demand ADPCM block decoding; there is no 2 MiB
  PCM cache to eliminate. The ARM9 currently mixes all voices and reverb, and
  ARM7/Maxmod plays the output. Investigate ARM7 work partitioning, contention,
  bounded queues and decode/mix costs. Hardware has fewer channels than the
  PSX's 24 voices, so assigning one DS channel per PSX voice cannot preserve it.
* **Streaming buffers need shared lifetime budgets.** Use bounded, sequential
  reads, prefetch at known loading boundaries and reuse staging space between
  mutually exclusive operations. Measure actual DLDI storage behavior; DSi SD
  throughput does not prove flashcart throughput. Movie video/audio buffers must
  be included before declaring a DS budget viable.

## Overlay code ranges are dead (2026-10-17)

Overwriting every overlay's MIPS code (the split config's `c` segments, about 850 KB over all overlays) right after
it loads leaves field 259 (including its encounter), Ark, Ifrit, Kuja and Zidane's Trance frame-identical, once the
36-byte data block at 0x800CF580 inside ovl_0b's code is kept (decomp-requests F33). So while an overlay is resident
its code range is free PS1 RAM: about 140 KB for the field overlay and 310 KB for battle's. Slot-0 overlays differ in
size, so the space is per mode. The hybrid's two primitive buffers (240 KB, battle only) now live in ovl_0b's code
range (emptied when overlay sectors land on them), which leaves 240 KB of main's code range free for state every
mode needs.

The VRAM shadow has no such slack: the game fills nearly all of PS1 VRAM (1000 KB of 1024 non-zero in field 259,
992 in Ark, 896 in Ifrit), and the CPU cannot read the DS texture banks while the 3D engine uses them.

## Effect modules load from SD (2026-10-24)

With 379 of 380 battle effect modules native, their code was 655 KB of the executable (`.text` 1.41 MB). The DS
build now links each module on its own at `psx_fx_area` (40 KB in bss; the largest module is 36 KB) against the
main executable's symbols (`--just-symbols`; `tools/fx_extern.py` keeps what they use through the main link's
`--gc-sections` and refuses one module using another's symbols), and packs them into `fxmods.bin`
(`tools/pack_fxmods.py`). `port/ds/fx_pack.c` reads the module the slot holds when `effect_native.c` identifies
it, as the PS1 reads it from the disc, then flushes the data cache and invalidates the instruction cache. The pack
carries the main executable's build id (new each link) and `psx_fx_area`'s address; without a matching pack the
modules can't run (the effect VM that was the fallback is removed: every module is native). The host build keeps
them linked in.

Result: `.text` 1,410,568 -> 732,640 bytes, `.rodata` 115 KB -> 88 KB, `.bss` 2.45 MB -> 2.39 MB (the 40 KB area
included). The total is about 4.7 MB against 4 MB. Battles (capture at 1761, PROFILE=2): screenshots identical,
and slightly faster (Ark 39.1 -> 38.9 ms per frame, Ifrit 39.7 -> 39.3, Trance 36.3 -> 36.1; Trance without the
pack, in the VM: 38.0). `fxmods.bin` goes in `ff9ds/` next to `raw/` (the build copies it into the test SD
folders); like the disc images, it is derived from game data and never committed.

## Code overlays load from SD (2026-10-24)

The decomp's code overlays (field 0a, battle 0b, world map 0c, Tetra Master 0d, the menus 32-3e and 65, ...) were all
linked into the executable: 447 KB, of which one slot's largest (battle, 174 KB) is ever needed at once. On the DS
their sections are prefixed `.psxovl_<id>` (Makefile, nsobj step), and `gen_link.py` writes OVERLAY statements that
the build splices into a copy of BlocksDS's ARM9 script (`build/ds/ff9ds_arm9.ld`, before the heap): each slot's
overlays share one address, outside the load segments, so ndstool leaves them out. Everything still resolves in one
link (NOCROSSREFS refuses calls between overlays of one slot). `tools/pack_overlays.py` extracts them into
`ovls.bin`; `port/ds/ovl_pack.c` reads an overlay's native code when the game loads its MIPS code
(`psx_overlay_patch`), then flushes the data cache and invalidates the instruction cache. A missing or stale pack
is fatal. `.text` 733 KB -> 286 KB; static memory ends at 2.83 MB, about 4.33 MB with the VRAM and SPU RAM shadows.
Black Waltz rewards on the DS (battle, rewards menu, back to the field) loads 0b, 65 and 0a and shows each correctly.

## Fitting the VRAM and SPU RAM shadows (2026-10-24)

The DS-mode build (`DSMODE=1 HYBRID=1 HWSOUND=1`) logs its free heap before allocating the shadows
(`port/psx/mem.c`). At the start of this work it had 1,275 KB for 1,536 KB of shadows. What made room, all without
changing what the game sees or hears:

* **Unused PS1 RAM, measured.** Probes on the host `mprotect` a psx_ram range once the game runs and play 135
  scenes (a field every 20th of the sweep, every 25th encounter, the boss and regression scenarios, 2500 frames
  each; `build/memuse/`): a touch faults with a backtrace. 0x1000 up is read in every battle (`g_btlUi->menuState`
  through a NULL `g_btlUi`: offset 0x12B4 of the BIOS's RAM), 0x2000-0xFFFF never. (The PS1 stack above the highest
  overlay looked unused too, but battles that reach their rewards load disc sectors to 0x801FCF00 and up: the probes
  ran 2500 frames, which is short of a battle's end. Longer runs are needed before trusting such a range.)
* **SPU RAM in pieces (HWSOUND).** The hardware mixer reads SPU RAM only a 16-byte ADPCM block at a time
  (`psx_spu_source_fetch`) and never runs the PS1 reverb, so SPU RAM can be kept in pieces with one compare per
  block: [0, 0xE000) in the BIOS's old RAM at psx_ram 0x2000, [0xE000, 0x16000) in the shared WRAM on an original DS,
  the rest on the heap. FF9's reverb work area ("studio large", 0x79020 up: the base register in all 135 scenes) is
  not kept: it reads as zeros, as it always did with hardware mixing, and no scene writes above it (a write there
  is dropped and logged once).
* **Shared WRAM.** DSMODE builds use their own ARM7 (`port/arm7/main.c`: BlocksDS's minimal core, linked with
  `__shared_wram_size=0`, all in the ARM7's 64 KB), so the ARM9 maps both shared WRAM banks (`WRAM_CR = 0`).
* **Main's data image (46 KB of .rodata) is read once, at boot**: it is now overlay 0xFF in `ovls.bin`, loaded into
  slot 0's place before the game starts.
* **The menus' and the movie player's overlays run in their own MIPS code range** in psx_ram (dead while they are
  resident; every native image is smaller than its MIPS code), linked there; `ovl_pack.c` reads the native bytes
  again wherever a disc sector of that overlay lands. Their two slots are gone (22 KB). Slot 0's overlays can't: in
  battle ovl_0b's code range holds the hybrid's buffers.
* **16 HWSOUND rings for 24 voices** (at most 16 can play): a voice holds a ring from its key-on while it sounds
  (64 KB of the spare range, which now holds the hybrid's UI lists, its block buffer and the display's text
  rectangles instead of bss).

* **The battle overlay is built with `-Os`** on the DS (`DECOMP_OPT_ds_ovl_0b`): it alone sets slot 0's size,
  174 -> 146 KB. Battle frame times stay within noise (48.31 -> 48.07, 31.26 -> 31.51, 37.87 -> 38.00 ms) with
  identical screens. `-Os` for all of the decomp's code would save 74 KB but costs up to 5.7% in fields.
* **The heap.** libnds's `sbrk` stops at `fake_heap_end`, which BlocksDS's crt0 sets 48 KB below the end of main
  RAM for a loader's return stub; the game never exits, so DS mode moves it up to the system area. The shadows are
  allocated first thing in `main`, as one block (the allocator doesn't grow a free block at the heap's end, so a
  second large block would need room for both).
* The host build `mprotect`s the reused PS1 RAM (`plat_guard_unused_ram`), so a scene that touches it crashes there
  instead of corrupting SPU RAM on the DS.

Result: 1,449 KB of heap for 1,420 KB of shadows (VRAM 1 MB and SPU RAM's heap piece 396 KB in one block); the
original DS runs with graphics and sound memory.

## Implementation order and acceptance

1. Remove inactive backend storage (done), then inventory resident code/data by
   subsystem and record runtime allocation peaks. Give the resident core, native
   overlay slots, PSX memory, renderer, audio, I/O and stacks explicit ceilings
   whose sum fits actual usable DS RAM with headroom.
2. Extract native overlays and relocated images into generated runtime files.
   Validate loading/unloading, relocation and callbacks on the DSi reference
   build first. Use direct scenario entry to exercise field, battle, world map,
   menus and transitions, including late-disc effects.
3. Resolve the remaining graphics/audio working set without changing observed
   PSX semantics. Measure CPU and I/O separately: existing 24-voice SPU stress
   measurements already exceed the real-time budget even at DSi speed, so a RAM
   fit alone cannot qualify the DS port.
4. Add a genuine DS linker/build configuration with overflow checks and clear
   startup failure if required memory cannot be allocated. The historical
   fallback that runs without graphics/sound memory is not successful DS support.
5. Run bounded scenarios in actual DS mode with one emulator at a time, then on
   DS hardware with DLDI storage. Require allocation headroom, no faults, matching
   PSX reference images/short animation videos, original simulation timing and
   continuous sound. Keep DSi as a tested configuration throughout.

Whether this fits with acceptable performance is still open. The design now
targets that possibility explicitly instead of assuming 16 MiB is available.

## Shared WRAM needs an MPU region (2026-10-25)

In DS mode BlocksDS's ARM9 protection regions cover main RAM up to 0x02FFFFFF but not 0x03000000, where the port
keeps SPU RAM 0xE000-0x16000 (shared WRAM, `WRAM_CR = 0`). The first access to it is a data abort; fields died about
a minute in (2110 at frame 821, 1102 at 1201), when a sound first used that part of SPU RAM, and the exception path
then overwrote all of main RAM (and DTCM) with a 128 KB pattern. `main` now maps the 32 KB with region 3 (the Slot-2
cartridge space, unused). The DSi's regions differ, so DSi runs were never affected. Tools from the hunt:
`ds_profile.py` saves a state when the game frame stops for 90 s (`*.timeout.ml8`; melonDS keeps ARM9 registers in its
`ARM9` section: R0-R15 at +24, CPSR after, then the banked FIQ/SVC/ABT/IRQ/UND registers); `BLACKBOX=1` keeps the
last 64 phases in DTCM; `CANARY=1` fits DS mode (8 KB tails); the status screen's row 9 shows the free heap.

## The 28 sample fields as an original DS (2026-10-25)

`build/validation/dsmode_bench.sh` (DSMODE=1 HYBRID=1 HWSOUND=1 FIELDGX=1, melonDS as a DS at 67 MHz with 4 MB, a
DLDI card per disc, PROFILE=2 frame times at frame 1500): all 28 ran their 1500 frames, none stalled. The times are
real ms per PS1 VBlank (the virtual frame), so the PS1's speed is 16.7 ms (fields wait `VSync(2)`, battles 4 VBlanks a
game frame): none is there (the fastest is 1811 at 19.7), 10 are at or under half of it (33.3 ms); the slowest are the scenes over the 3D engine's limits (931 83 ms, 1812 118 ms, drawn in
software) and the large 3D scenes (955, 2110 66 ms, 1508 62 ms). Speed is now the original DS's gap.

| field | ms/frame |
|---|---|
| d1_f0158 | 22.0 |
| d1_f0110 | 35.5 |
| d1_f0615 | 25.6 |
| d1_f0453 | 54.3 |
| d1_f0306 | 30.4 |
| d1_f0205 | 35.2 |
| d1_f0162 | 32.8 |
| d2_f0931 | 83.4 |
| d2_f1425 | 34.9 |
| d2_f1203 | 31.4 |
| d2_f1414 | 58.5 |
| d2_f1102 | 41.2 |
| d2_f0955 | 66.0 |
| d2_f1057 | 46.8 |
| d3_f2714 | 33.6 |
| d3_f1551 | 25.9 |
| d3_f1811 | 19.7 |
| d3_f1508 | 61.5 |
| d3_f1812 | 117.7 |
| d3_f1606 | 28.8 |
| d3_f2055 | 39.7 |
| d4_f1706 | 54.5 |
| d4_f2934 | 30.6 |
| d4_f1953 | 26.0 |
| d4_f1911 | 35.8 |
| d4_f0351 | 50.6 |
| d4_f2110 | 66.5 |
| d4_f2162 | 53.4 |

## 24 boss battles as an original DS (2026-10-25)

The boss fixtures (`build/validation/boss_sweep_fixtures`, each the encounter entered directly) through the same
script: all 24 ran their 1500 frames, none stalled. None reaches even half the PS1's speed (33.3 ms per PS1 VBlank; the PS1's is 16.7) yet (42-99 ms); the effect-heavy
late bosses are slowest (Ozma 99, Lich 90, Nova Dragon 88, Trance Kuja 85, Earth Guardian 83, Hades 80).

| boss battle | ms/frame |
|---|---|
| d1 beatrix | 46.4 |
| d1 black_waltz | 42.0 |
| d1 garuda | 53.1 |
| d1 gizamaluke | 45.8 |
| d1 plant_brain | 54.7 |
| d2 antlion | 59.3 |
| d2 hilgigars | 55.0 |
| d2 lani | 56.6 |
| d2 ralvuimago | 55.8 |
| d2 soulcage | 52.5 |
| d2 tantarian | 43.8 |
| d2 zorn | 58.2 |
| d3 earth_guardian | 82.6 |
| d3 silver_dragon | 56.7 |
| d4 deathguise | 50.2 |
| d4 hades | 79.5 |
| d4 kraken | 51.2 |
| d4 lich | 90.4 |
| d4 maliris | 51.8 |
| d4 necron | 54.6 |
| d4 nova_dragon | 88.1 |
| d4 ozma | 99.3 |
| d4 tiamat | 53.8 |
| d4 trance_kuja | 85.0 |

### Where a slow battle's time goes on an original DS

Ozma (PROFILE=1, 104 ms a frame, no VBlank wait: CPU-bound; a scene is submitted every ~3 frames): per scene
46 ms building the GX list (`emit_scene`), 31 ms copying textures (256 KB: the effects rewrite 4 whole regions,
so copying only written rows doesn't help), 7 ms planning; per frame ~20 ms on the 2D layer, 17 ms in effect
callbacks, 14 ms GTE, 16 ms software rasterising (the 2D layer's primitives). (A first reading of the status
screen's drop count as Ozma's per scene was wrong: it counted the whole run. At frame 1500 Ozma's scene is 1768
primitives, 42 over the vertex RAM; the split frame below draws those in software.)

### Ozma sampled as an original DS (PROFILE=3 in DS mode, 2026-10-25)

With ITCM counted (earlier samples there were taken for idle time) and 1 KB buckets: GTE emulation ~11% (`rtp_sf1`
6%, `gte_cmd`, `mat_mul`, `interpolate`), the 2D layer ~9% (`ds_scale_display_text_pair` 5%, `gxh_present` 4%), the
software rasteriser ~7% (the battle windows' primitives), the GX path ~17% (`emit_scene` 4.8, GP0 `command` 4.5,
`upload` 3.8, `plan_textures` 2.1, `translucent_id` 1.2), interrupts ~3% (the sampler's own included). Two things
tried that the DSi's profile suggested did nothing on a DS (copying only a texture region's written rows; reusing
a buffer's 2D-layer block hashes across switches). The time is spread: matching the PS1 in heavy battles on a 67 MHz
ARM9 means cutting all of these, the exact GTE included.

## 30 random fields as an original DS (2026-10-25)

Fields drawn at random (seed 25, `build/validation/dsmode_rand30.list`) beyond the 28 samples: 29 ran their 1500
frames; 2101 died with "psx_spare_alloc: 32768 bytes over the spare range" (the block runs' tables had filled the
spare range; moved after the first field buffer) and then ran too (99 ms). Frame times 16.7-130 ms: d1_f0111 36.9296, d1_f0259 83.7146, d1_f0312 24.2382, d1_f0573 46.6887, d1_f0600 38.7511, d1_f0762 28.5198, d1_f0767 129.8035, d1_f1256 25.0598, d2_f0706 36.1133, d2_f1012 38.3618, d2_f1211 37.7321, d2_f1250 54.3212, d2_f1309 44.5343, d2_f1407 39.6985, d2_f1800 47.3714, d3_f1600 19.1103, d3_f1602 35.9376, d3_f1810 54.3261, d3_f2111 37.4821, d3_f2152 63.5959, d3_f2159 42.3015, d3_f2255 16.7151, d3_f2655 48.7039, d3_f2713 25.0808, d4_f1700 27.4269, d4_f1913 40.1966, d4_f2910 26.0675, d4_f2913 41.7072, d4_f2951 52.72 ms.

### Boss battles against the host's frames (2026-10-25)

The 24 boss screenshots beside the host renderer's frame 1500 showed: black boxes behind every battle's HP and MP
numbers (a fill running past its span after the STMIA change: fixed, `fill_bursts`); spell effects whose additive
light saturates toward white on the PS1 come out coloured with the 3D engine's approximation (Maliris's explosion
orange instead of white, Tiamat's beam purplish, Necron's glow bluish, Kraken's glow missing): battles have no exact
additive layer (their glows are too large to draw in software affordably), so this needs the capture pass; text at
the 0.8 scale is heavier ("Meteor"). The fields' 58 screenshots matched apart from field 767's rain (fixed) and
additive lights (2910: fixed by the exact layer).

## 30 random battles as an original DS (2026-10-25)

Ordinary encounters drawn at random (seed 26, `build/validation/dsmode_battle30.list`), each to game frame 1500: all
30 ran without a crash or stall. Frame times 46.2-68.8 ms, mean 54.2 per PS1 VBlank (the PS1's speed is 16.7: these battles run at about 31% of it, a 15 fps game
frame taking about 217 ms instead of 67): d1_b0032 48.9,
d1_b0182 47.5, d1_b0186 58.5, d1_b0187 53.9, d1_b0230 54.2, d1_b0238 60.8, d1_b0296 46.2, d2_b0091 57.8, d2_b0098 68.8,
d2_b0102 51.7, d2_b0140 62.7, d2_b0229 54.4, d2_b0331 50.3, d2_b0342 59.7, d2_b0355 51.7, d2_b0647 48.1, d2_b0678 50.9,
d2_b0840 50.2, d2_b0844 53.1, d3_b0216 50.0, d3_b0837 65.1, d3_b0905 54.3, d4_b0137 59.3, d4_b0215 54.1, d4_b0223 58.8,
d4_b0586 53.6, d4_b0664 50.4, d4_b0686 46.7, d4_b0839 53.7, d4_b0919 49.1 ms.

### Where an ordinary battle's time goes (d2_b0098, PROFILE=3 in DS mode, 2026-10-25)

By object file (1 KB buckets; `PCLO=0x... PCSHIFT=4` samples a 40 KB window in 16-byte buckets and ITCM in 32-byte
ones, which separates neighbours the 1 KB buckets mix: the "9% in the text scaler" was the VBlank wait beside it in
ITCM): the renderer `gx_hybrid` 29% (`emit_scene` 6.8, `gxh_present` 4.9, `plan_textures` 4.1, `hybrid_poly` 2,
`regions_touch` 1.6), the GTE 14% (`rtp_sf1` 7.2, `gte_cmd` 4.2, `mat_mul` 2), the software rasteriser (the 2D layer's
primitives) 7%, GP0 decoding 7% (about 3 us a primitive), the text scaler 2.3%, `gpu.c` 3.4%, the game's own code
8%, idle in the VBlank wait about 9%. The port's layers are about 70% of the time. Compiling `emit_scene`,
`plan_textures` and `gxh_present` as ARM (3 KB more) changed nothing measurable (68.79 ms both ways): reverted.

### VSync counts the VBlanks a slow frame took (2026-10-25)

`plat_vsync_begin` (display.c): a VSync call's frames count the real VBlanks that passed while the game worked, as
the PS1's `VSync(n)` does, instead of giving the debt up past `MAX_LAG` and waiting again. Virtual time is
unchanged. d2_b0098 68.8 -> 68.4 ms, d1_b0186 58.5 -> 58.3, d1_f0110 35.5 -> 33.7, d4_f2110 66.5 -> 63.8.

### Approximate geometry: RTPS/RTPT in 32 bits (2026-10-25)

The user's choice for the speed gap: geometry may be approximate on the original DS. First step, `PORT_GTE_APPROX`
(DSMODE=1 builds): the sf=1 perspective transform in 32-bit arithmetic. (TR << 12 + M * V) >> 12 is TR + (M * V >> 12)
exactly, so it differs only where M * V passes 2^31, and it never sets MAC1-3's 44-bit overflow flags. Against the
exact code over 2 million random RTPT+RTPS with FF9-like ranges (matrix entries within 1.0, vertices within 4000,
translations within 32000): no output register differs; with full-range random matrices and vertices 0.19% do.
Screenshots at frame 1500 are identical. 1.5-2.7% faster: d2_b0098 68.4 -> 67.3 ms, d1_b0186 58.3 -> 57.3, d4_f2110
63.8 -> 62.0.

### Battle UI drawn once a game frame (2026-10-25, the user's choice of renderer shortcuts)

The battle's window layer is drawn by the game at every VBlank (`btlui_vsync` -> `btlui_submitOt`, through
`ContinueDraw`), four times a 15 fps game frame, over the shown buffer. `psx_skip_vblank_list` (display.c) lets
`ContinueDraw` draw it at every `UI_RATE`-th VBlank (4) and whenever the shown buffer changes; the UI's logic (input,
gauges, counters) still runs at every VBlank. It looks the same (gauges and the cursor move at 15 fps). Every 2nd
VBlank: d2_b0098 67.3 -> 63.7 ms, d1_b0186 57.3 -> 53.7; every 4th: 60.0 and 50.7 (11%).

### Measuring a whole battle (2026-10-25)

The default timing block is the last 128 virtual frames before the capture frame, which in the battle fixtures is
often the heaviest effect of the fight (d2_b0098 57.5 ms there, 36.3 over the last 1024 frames). `PROFWIN=1024`
times the last 1024 frames instead (most of the battle), and changes now get compared with it. The sampling profiler
(PROFILE=3) keeps counting while the capture frame waits for the state to be saved, about 20% of a run's samples
in `swiIntrWait` from `plat_wait_frame` (now visible: its waits are counted per caller, `--wrap=swiIntrWait`, and
the ITCM buckets are 32-bit, which had saturated). The real pacing waits came to 1.8 s of a 77 s run.

### GTE: NCLIP and AVSZ in place, model vertices projected in a batch (2026-10-25)

NCLIP and AVSZ3/AVSZ4 (half the battle's GTE commands, from meshNclip and meshAvsz3) are computed inline from
`gte.h` when the opcode is a constant, exactly (3 million random register files: identical). `btl_projectMesh` is
overridden (port/game/mesh_native.c): each bone's matrix still goes through the GTE; its vertices go through
`gte_rtpt_batch` (32-bit, the matrix in registers), and the last group through the GTE again so the registers end
as before. With one VBlank of pacing slack (a late frame's `VSync(0)` doesn't wait again), over 1024 frames:
d2_b0098 37.3 -> 36.3 ms, d1_b0186 34.5 -> 33.6 (2.7%); fields d1_f0110 33.7 -> 32.5, d4_f2110 62.0 -> 60.0.

## 100 more random fields as an original DS (2026-10-25)

Fields drawn at random (seed 27) from the 1073 not run before (`build/validation/dsmode_rand100.list`): all 100 ran
their 1500 frames without a crash or a stall (the first 40 on 9e5b579's parent, the other 60 on 9e5b579). Frame
times (last 128 frames, ms per PS1 VBlank) 18.2-94.4: d1_f0057 52.4, d1_f0103 58.2, d1_f0104 37.0, d1_f0108 40.3, d1_f0109 34.0, d1_f0161 40.1, d1_f0254 20.1, d1_f0303 32.0, d1_f0451 27.0, d1_f0455 29.9, d1_f0507 28.7, d1_f0557 94.4, d1_f0560 48.2, d1_f0565 33.6, d1_f0603 82.3, d1_f0608 24.5, d1_f0619 42.3, d1_f0655 18.2, d1_f0702 38.3, d1_f0758 71.1, d1_f0802 56.3, d1_f0850 24.1, d2_f0657 25.1, d2_f0663 21.4, d2_f0750 58.2, d2_f0852 71.5, d2_f0906 40.2, d2_f1001 24.2, d2_f1002 36.7, d2_f1014 58.1, d2_f1152 43.0, d2_f1205 47.7, d2_f1215 45.7, d2_f1222 29.8, d2_f1227 19.6, d2_f1306 37.6, d2_f1307 44.3, d2_f1311 30.8, d2_f1355 69.1, d2_f1408 44.1, d2_f1452 34.4, d2_f1508 56.8, d2_f1509 35.0, d2_f1656 54.8, d2_f1758 52.2, d2_f1951 31.5, d3_f0350 40.0, d3_f0351 48.2, d3_f0353 45.3, d3_f0354 37.2, d3_f0663 21.3, d3_f0701 47.7, d3_f0769 62.4, d3_f1425 40.9, d3_f1451 24.1, d3_f1555 19.8, d3_f1608 24.7, d3_f1758 52.2, d3_f1814 19.0, d3_f1916 58.2, d3_f2003 23.1, d3_f2006 21.0, d3_f2052 47.0, d3_f2053 60.2, d3_f2109 43.9, d3_f2151 32.2, d3_f2162 50.6, d3_f2168 41.4, d3_f2170 47.4, d3_f2205 48.2, d3_f2214 20.6, d3_f2305 22.6, d3_f2350 32.2, d3_f2401 73.8, d3_f2406 48.5, d3_f2508 30.4, d3_f2512 45.5, d3_f2654 53.3, d3_f2708 35.1, d3_f2750 44.1, d3_f2802 51.1, d3_f2952 36.9, d3_f3100 72.5, d4_f0703 82.4, d4_f0707 46.1, d4_f1900 41.8, d4_f1912 48.7, d4_f1952 32.0, d4_f2102 51.1, d4_f2108 32.2, d4_f2152 61.4, d4_f2404 63.6, d4_f2405 34.1, d4_f2451 41.1, d4_f2458 60.2, d4_f2750 44.1, d4_f2754 43.4, d4_f3002 20.8, d4_f3008 44.1, d4_f3010 53.2.

## 200 more random fields as an original DS (2026-10-25)

Fields drawn at random from those not run before (`build/validation/dsmode_rand200.list`), 1500 frames each: 198 ran
without a crash or a stall. d2_f1403 and d2_f1404 (Fossil Roo) crashed at frame 300: `file_copyViaVram` copies a file
through the drawing buffer (LoadImage, then StoreImage back), and the read replayed the buffer's older recorded scene
over the loaded data. Field buffers now keep the rectangles written since their scene was recorded (`Buffer.over`):
both run to frame 600 since e76ffc0 (51.2 and 43.9 ms). The other 198, last 128 frames, ms per PS1 VBlank (average
49.1): d1_f0053 31.7, d1_f0064 66.0, d1_f0066 54.3, d1_f0114 44.6, d1_f0117 92.2, d1_f0151 63.8, d1_f0153 32.8, d1_f0258 83.9, d1_f0305 35.0, d1_f0307 45.3, d1_f0309 55.2, d1_f0354 38.0, d1_f0401 70.1, d1_f0403 44.4, d1_f0404 40.5, d1_f0452 29.1, d1_f0454 48.3, d1_f0505 28.8, d1_f0551 93.6, d1_f0559 58.7, d1_f0567 45.4, d1_f0572 95.6, d1_f0604 69.7, d1_f0605 67.0, d1_f0606 42.2, d1_f0607 37.2, d1_f0612 42.5, d1_f0613 58.1, d1_f0651 31.2, d1_f0660 25.0, d1_f0704 26.9, d1_f0753 43.4, d1_f0756 50.7, d1_f0763 57.0, d1_f0766 73.1, d1_f0800 33.8, d1_f0813 59.2, d1_f0814 51.8, d1_f0852 71.3, d2_f0351 49.3, d2_f0353 46.3, d2_f0357 58.0, d2_f0455 30.7, d2_f0456 29.8, d2_f0602 63.4, d2_f0651 31.2, d2_f0655 18.7, d2_f0661 37.6, d2_f0702 38.6, d2_f0752 76.9, d2_f0755 33.8, d2_f0759 66.7, d2_f0764 36.5, d2_f0765 40.9, d2_f0767 78.7, d2_f0803 33.4, d2_f0807 38.8, d2_f0810 93.1, d2_f0812 54.1, d2_f0902 45.9, d2_f0908 73.4, d2_f0911 31.1, d2_f0953 46.9, d2_f1006 45.5, d2_f1013 48.1, d2_f1051 61.8, d2_f1055 47.2, d2_f1058 52.5, d2_f1150 47.7, d2_f1204 28.2, d2_f1210 54.0, d2_f1214 89.5, d2_f1223 27.0, d2_f1251 29.5, d2_f1303 42.3, d2_f1359 36.2, d2_f1360 85.3, d2_f1366 24.4, d2_f1368 19.2, d2_f1403 1.0, d2_f1404 1.0, d2_f1409 43.7, d2_f1419 36.2, d2_f1424 70.8, d2_f1450 26.3, d2_f1456 47.7, d2_f1458 33.3, d2_f1459 48.6, d2_f1460 41.4, d2_f1463 36.8, d2_f1501 37.2, d2_f1554 18.5, d2_f1557 25.0, d2_f1600 19.1, d2_f1605 65.6, d2_f1608 25.3, d2_f1653 43.8, d2_f1659 60.8, d2_f1752 56.5, d2_f1754 62.3, d2_f1756 74.7, d2_f1953 27.0, d3_f0355 46.7, d3_f0358 27.0, d3_f0450 35.4, d3_f0457 62.6, d3_f0703 84.3, d3_f0751 61.0, d3_f0763 57.0, d3_f0764 36.5, d3_f0801 56.1, d3_f0803 33.4, d3_f0853 40.8, d3_f1454 51.9, d3_f1455 38.4, d3_f1456 47.7, d3_f1459 48.6, d3_f1504 27.1, d3_f1509 36.1, d3_f1552 26.6, d3_f1607 45.8, d3_f1609 45.3, d3_f1803 62.4, d3_f1809 26.8, d3_f1850 56.8, d3_f1857 40.2, d3_f1902 49.1, d3_f1912 50.0, d3_f2001 25.8, d3_f2002 27.1, d3_f2050 67.1, d3_f2103 42.0, d3_f2160 98.2, d3_f2169 65.1, d3_f2202 30.7, d3_f2203 73.2, d3_f2207 26.1, d3_f2213 26.5, d3_f2253 28.4, d3_f2300 51.9, d3_f2301 65.3, d3_f2302 108.6, d3_f2303 118.0, d3_f2304 71.9, d3_f2351 16.8, d3_f2355 40.3, d3_f2363 16.7, d3_f2365 47.2, d3_f2402 51.8, d3_f2404 65.4, d3_f2452 60.3, d3_f2513 32.5, d3_f2553 53.0, d3_f2605 42.8, d3_f2608 27.8, d3_f2650 32.6, d3_f2700 52.2, d3_f2706 32.3, d3_f2716 90.2, d3_f2852 24.0, d3_f2853 30.4, d3_f2855 95.8, d3_f2856 92.9, d3_f2950 36.7, d3_f2953 65.5, d3_f2954 90.2, d4_f0352 33.9, d4_f0655 18.7, d4_f0662 22.7, d4_f0750 58.0, d4_f0759 66.7, d4_f1701 44.3, d4_f1906 43.6, d4_f2103 42.0, d4_f2104 42.2, d4_f2111 38.0, d4_f2112 29.2, d4_f2158 36.4, d4_f2454 58.2, d4_f2503 21.4, d4_f2508 31.2, d4_f2513 32.5, d4_f2752 152.9, d4_f2756 53.0, d4_f2800 49.2, d4_f2901 42.0, d4_f2904 83.7, d4_f2911 22.3, d4_f2912 41.0, d4_f2918 28.2, d4_f2919 50.5, d4_f2922 153.8, d4_f2923 40.6, d4_f2924 35.2, d4_f2928 130.2, d4_f2952 37.7, d4_f2954 90.2, d4_f3001 29.7, d4_f3005 53.5, d4_f3011 52.1.

## Where a field's time goes, and what didn't help (2026-10-25)

Original DS (melonDS DS mode, JIT), PROFWIN=1024, ms per PS1 VBlank (a 30 fps field has two per game frame):

| | d3_f2205 | d1_f0110 | d4_f2110 |
|---|---:|---:|---:|
| after this round (6deb08d, d4cf338) | 46.9 | 31.4 | 55.7 |
| background tiles not drawn (experiment) | 37.4 (before d4cf338) | 23.9 | |
| model polygons not linked (experiment) | 25.8 | 19.7 | 22.3 |

Field 2110 with its own timers in `submit` (per VBlank): the OT walk 19.5 ms, planning (`field_frame`) 6.8, GX
emission 10.8, texture upload 0.1; about 1,240 model polygons and 405 background tiles a game frame, so roughly
24 us per primitive in the walk and 13 in emission. The cost is spread over the whole per-primitive pipeline
(decode, record, plan, emit), about 1,000 instructions a primitive in all.

In this melonDS's model (checked in its source: the KHMelonMix fork's CP15.cpp and JIT) cached main-RAM data
accesses cost a flat 3 cycles and main-RAM Thumb code about 1.5-3 cycles an instruction, ITCM ARM code 1: cost
follows instruction and memory-operation counts, not locality. Real hardware differs (cache hits take 1 cycle,
misses much more); only a DS can say by how much.

Tried and not kept:
- Model polygons by reference (fldmdl_drawMesh wrapped; the walk notes each opaque model polygon's packet and its
  place among the recorded primitives instead of recording it; planning and emission decode the packet): the walk
  went from 19.5 to 15.0 ms, planning from 6.8 to 9.0 and emission from 10.8 to 13.6 (decoding a packet twice
  costs more than reading a recorded primitive twice): no gain, reverted.
- Not sending the scene to the GX FIFO at all (experiment): 0.7 ms, so emission doesn't wait on the hardware.

Kept: background tiles' lean path (2-4%), batched model projection (4-7%), all with identical frames.

## World maps (2026-10-25)

`build/validation/wmsweep/cfg`: world maps 9000-9012 on each disc (discs 1-2 without a story counter; disc 3 with
`story=10000`, without which its encounters reach for battle data the disc doesn't have; disc 4 with `story=11100`,
whose world data is the late one). Host, 3000 frames with `tools/walk_presses.py` input from frame 300: 52/52 after
one fix (43 run to the end; 9 walk into a field another disc holds, which the scenario layer reports). The fix: a
battle's Item command confirmed on an empty list reads the NULL entry before `btlui_selectItem`'s own check (world
9011, disc 4): a counted `PSX_NZ` patch. Original DS, 20 of them (9000, 9003, 9006, 9009, 9012 on each disc), walking
from frame 300 to 1500: all pass after a second fix: the software rasteriser's palette cache (32 KB, allocated the
first time it's needed) didn't fit in the spare PS1 RAM once a battle had allocated everything else (battle 46's
menus, disc 4); it has 16 slots now (16 KB), with no change in time or frames on d3_f2205 and d2_b0098.

World maps are drawn in software (gx_hybrid has no place for them): 140-216 ms per VBlank, about 10x the PS1.

## With input: battle menus and the main menu (2026-10-25)

The 30 random battles again on an original DS, now with `tools/walk_presses.py` input (Cross every 0.75 s opens
commands, items and targets): 30/30 after the spare-RAM fix (20fb4d0, f9a4ff8). `tools/menu_presses.py` (the main
menu opened every 450 frames and wandered through): the host ran 100 fields x 3000 frames with it without a crash,
and an original DS 10 fields x 1500 frames. The main menu costs 56-115 ms per VBlank on an original DS (the Config
screen at capture: 109 ms): left for the dual-screen redesign, which moves these screens to the bottom screen.

## The world map native (WORLDGX=1, 2026-10-25)

The user's choice of 2026-10-25: native drawing with the 3D hardware's fog. The world map was drawn in software
(gx_hybrid had no place for it): 140-216 ms per VBlank, about 10x the PS1. About 3300 primitives a frame, over the 3D
engine's 2048: ~2400 terrain faces (a lit POLY_GT3 each, and a POLY_G3 of fog colours added over it, blend mode 1),
the backdrop (sky dome, 280 faces at one OT entry), models, effects, the navigation map.

- A place for ovl_0c in gx_hybrid (`port/include/ovl0c_layout.h`: two buffers of 560 primitives, their tables and depths,
  three frames' terrain in ovl_0c's dead code range).
- `wm_drawBlock` and `wm_drawBackdrop` are overridden (`port/game/world_native.c`): after the game's vertex pass,
  `port_native_world_block` keeps the projected vertices and the faces `wf_face` would draw (its culling, OT range,
  half-resolution and alternate pages), instead of the face routines' packets.
- Fog: the PS1's lit colours are darkened toward a black far colour by depth and the fog triangle adds the fog colour
  by depth, which is the 3D engine's fog (colour x (1 - d) + fog x d) with each vertex's colour lit / (1 - d). A face
  with only the fog triangle is a black polygon fogged. The fog table follows the vertex pass's ramp. Vertices of the
  second fog entry get the first's ramp. The backdrop's fog is the alpha ramp's (fog colour, alpha d), the
  translucent fog polygons under one ID.
- The scene is depth tested: every other primitive at its OT entry's depth (opaque ones from the last, so the first of
  equal depths drawn is the PS1's topmost; translucent ones in order just in front), terrain vertices at their own.
  A terrain face reaching past the backdrop's OT entry stays behind it (the PS1 draws it first); faces the GPU wouldn't
  draw (1024+ wide, 512+ high) are left out.
- Colours over x1 (the PS1 modulates up to x2, the DS x1): a texture use with such vertices gets its palette pre-scaled.
- Over the 3D engine's polygons and vertices (the sea: ~2060 faces), fog-only faces go first, then lit ones, smallest
  first in each.
- A frame's terrain is needed until its scene is sent two frames later (the world map shows a buffer before walking
  its new OT into it): three slots by OT generation.
- Palettes generally: TEXIMAGE's colour-0-transparent bit only where the CLUT's first colour is 0x0000 (the sea's CLUT
  has a colour there; fields' and battles' frames unchanged).

Original DS, PROFWIN=1024: d1_w9009 167 -> 36.5 ms, d3_w9000 37.8, d3_w9006 45.7, d4_w9000 37, d4_w9006 41.2 (2-3x the
PS1's 16.7 at 20 fps instead of 10x). Against the host's frames: fog worlds, the clear sky and both seas close.
With walking input from frame 300 (`tools/walk_presses.py`, encounters and all), the 20 world maps of the earlier
sweep all reach frame 1500 (two, d4_w9000 and d4_w9009, inside a battle with no complete timing block): 20.6-45.8 ms
per VBlank where timed. d4_w9000 had stalled at frame 1453 in software.

## Field models native (FIELDGX=1, 2026-10-25)

The user's choice of 2026-10-25 (native drawing, per-pixel depth). Model polygons were most of a field's cost (the
experiment above: d4_f2110 55.7 -> 22.3 ms without them).

- The field's primitives are depth tested, each at its OT entry's depth (as the world map's: opaque ones from the last
  of an entry, translucent ones in order just in front).
- `fldmdl_drawMesh` is wrapped (`port/game/field_mesh.c`, renamed `_link`): `port_native_field_mesh` takes the group's
  vertices as `fldmdl_projectMesh` left them (x and y in 11 bits each, as the GTE saturates them, and 10 bits of depth
  from the group's nearest OT entry, 4 bytes a vertex), culls its polygons as `model_link.c` does (NCLIP unless
  double-sided) and keeps their bits. The scene is sent two frames later: three slots by OT generation, in the rest
  of ovl_0a's dead code range (3456 vertices a scene; field buffers at most 1500 primitives, as a scene with its
  models native records about 400-900).
- A group's translucent polygons are linked into the OT as `model_link.c` does (`link_translucent`), so they keep
  the PS1's order; its opaque ones are drawn natively, each vertex at its own depth on the OT entries' scale.
- The 3D engine's budget counts the polygons actually drawn (an estimate of half sent d4_f2110 into software).
- A group the game freed or reused before its scene is sent (a field change) is skipped (its polygon records and
  counts as taken; an index past its vertices), and a sector loaded over the ring empties it: d1_f0456 crashed at
  frame 368 without this.
- A buffer that fills goes software for that scene only: the 60-scene hold kept the models in packets, which filled
  the next buffer too. The snapshot doesn't look at the buffer's soft flag or the GPU's backend (both the last
  scene's).
- Memory: port code only an overlay calls lives in that overlay's native area (`PORT_OVERLAY_CODE`, native_mesh.h):
  the world map's overrides in ovl_0c (slot 0 is sized by ovl_0b), the field models' hooks and their emission (ARM,
  one loop per polygon kind) in ovl_0a, whose tail the 1500-primitive buffers leave free. The DS-mode heap had only a
  few KB over the shadows: a 6 KB resident growth stopped the build at boot ("no room for the VRAM/SPU shadows").

Original DS, PROFWIN=1024, ms per PS1 VBlank: d4_f2110 57.9 -> 36.2, d3_f2205 48.9 -> 33.8, d1_f0110 32.5 -> 24.2;
screens as the host's.

Measuring: PROFILE=3 now counts from CAPTURE_FRAME / 2 to the capture (it counted the load and the 16 s wait for
the state, which made the VBlank wait look like a third of the frame), and its window build (PCLO) fits DS mode's
heap. `ds_profile.py` calls a frame-0 stall after 300 s (a fatal stop at boot used to wait out the 45-minute timeout).
In d4_f2110 skipping the background tiles' planning and emission saves only 3.5 ms (44.6 -> 41.1): the rear plane
would buy less than its cost suggested.

### What the field background costs, and the plan (2026-10-25)

- Dropping every opaque SPRT_16 in fields at GP0's decoder (experiment, the picture without its background): d4_f2110
  36.3 -> 28.8 ms, d3_f2205 33.9 -> 24.5, d1_f0110 24.2 -> 17.3. The background's tiles cost 7-9 ms a VBlank across
  the walk, recording, planning and emission (skipping only planning and emission saved 3.5).
- Fields over the 3D engine's budget go software (91-122 ms: d1_f0504, d1_f0563, d1_f0574 of the 20 swept). Vertices
  are the limit (6144): d1_f0504 6864 with 607 background sprites (2428 vertices), d1_f0563 6705 with 425, d1_f0574
  9656 with 455 (1602 model polygons with 4997 vertices: over the budget even without its background).
- The composite (9bd7a96): the background sprites drawn by the software renderer into the scene's frame buffer,
  sampled into bank A's bitmap behind the transparent 3D layer; those covering no 3D primitive or model group in
  their 16-pixel cells leave the 3D scene. Only for scenes past the 3D engine's budget (60 scenes after one was):
  d1_f0504 89.4 -> 64.2 ms, d1_f0563 93.7 -> 47.2. For scenes within it, it doesn't pay: planning and emission saved
  about 1.3 ms in d4_f2110 against the hashing, and backgrounds change often (cells changed per scene with walking
  input: d1_f0504 about 190 of 280, all scrolling; d3_f2205 136 large and 179 small changes in 551 scenes; d1_f0563
  536 of 695 static), each change a redraw (a full one about 15 ms; drawing per pixel in C took 130).
- What's left of the background's 7-9 ms is the walk (GP0 decoding and recording each sprite), which only a hook in
  `fldbg_drawOverlay` could skip. The rear plane (banks C and D) can't be rewritten while the 3D engine reads it.

## The walking sweep resumed (2026-10-26)

`build/validation/dsmode_walk200_rest.list` with walking input (`tools/walk_presses.py 300 1500 7`), on the native
field models and the composite: its first 60 fields reach frame 1500 without a crash (d1_f0652 walks into field 659,
which its fixture doesn't have; a few end inside a battle or a notice screen, without a whole timing block). Found on
the way:
- d1_f0456 crashed at frame 368 (a field change: a scene's model groups freed before it was sent; d19d01c).
- Software frames showed black, or the last 3D frame's models (the world map's clear quad drawn for fields; f9903d4).
- A battle after a field encounter ran with the field's place, all in software (97 ms -> 35.8; b480e41).
- The field background's merged tile packets missed the short path (1.3-1.7 ms a field; 87951ba).

Fields 61-100: no crash of the port's. d3_f0455 and d3_f0855 stopped in `btlbg_init` with a NULL battle background:
disc 3's encounters without a story counter reach for battle data the disc doesn't have (as the world maps above);
the disc 3 field fixtures now have `story=10000` (but d3_f2205, whose host reference was made without), and both
reach frame 2000. Others stopped on fields their fixture doesn't have (617, 761, 1422).

Fields 101-179 (the end of the list): 74 timed, 4 more reach frame 1500 inside a battle. d4_f3100 walked onto the
world map without a story counter and its NULL writes reached the ITCM mirrored at address 0 (a jump to 4 from the
IRQ handler); the disc 4 field fixtures now have `story=11100` (but d4_f2110), and it reaches frame 1500. So all 200
fields of the walking sweep run on an original DS without a crash of the port's.

## 54 battles after the architecture levers (2026-10-25)

The 24 bosses and the 30 random battles again on 0f71760 (UI drawn once a game frame, GTE register file in DTCM,
32-bit projection, NCLIP/AVSZ inline, models and background drawn natively in the PS1's order): all 54 ran their 1500
frames without a crash or stall. Last 128 frames, ms per PS1 VBlank: random battles 54.2 -> 36.7 on average (32 per
cent), bosses 61.1 -> 44.3 (27 per cent); the frames match the earlier runs apart from small differences in additive
effects. d1_boss_beatrix 31.2, d1_boss_black_waltz 29.0, d1_boss_garuda 36.4, d1_boss_gizamaluke 32.9, d1_boss_plant_brain 36.6, d2_boss_antlion 40.1, d2_boss_hilgigars 37.2, d2_boss_lani 36.9, d2_boss_ralvuimago 39.9, d2_boss_soulcage 36.6, d2_boss_tantarian 29.0, d2_boss_zorn 35.1, d3_boss_earth_guardian 69.1, d3_boss_silver_dragon 37.5, d4_boss_deathguise 36.4, d4_boss_hades 58.6, d4_boss_kraken 35.3, d4_boss_lich 68.8, d4_boss_maliris 32.4, d4_boss_necron 38.9, d4_boss_nova_dragon 72.1, d4_boss_ozma 86.0, d4_boss_tiamat 34.4, d4_boss_trance_kuja 72.5, d1_b0032 32.0, d1_b0182 28.3, d1_b0186 44.7, d1_b0187 37.1, d1_b0230 37.8, d1_b0238 43.9, d1_b0296 31.9, d2_b0091 40.8, d2_b0098 43.0, d2_b0102 32.9, d2_b0140 47.9, d2_b0229 37.0, d2_b0331 35.1, d2_b0342 34.7, d2_b0355 35.1, d2_b0647 32.5, d2_b0678 35.3, d2_b0840 34.4, d2_b0844 36.2, d3_b0216 33.5, d3_b0837 44.4, d3_b0905 36.6, d4_b0137 36.1, d4_b0215 36.1, d4_b0223 40.1, d4_b0586 37.1, d4_b0664 34.5, d4_b0686 30.8, d4_b0839 36.9, d4_b0919 33.3.
