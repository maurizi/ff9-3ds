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

## Where a field's time goes now (2026-10-26)

Original DS, `dsmode_bench_w.sh` fixtures, ms per PS1 VBlank (PS1: 16.7): d1_f0110 21.7, d3_f2205 31.0,
d4_f2110 34.9. The sampler, grouped by module (d1_f0110), puts about 63% of the frame in rendering:
- gx_hybrid.c: 33%.
- The field models' emit (`fm_emit`, ovl_0a's area): 9.5%.
- `field_frame`: 3.5%.
- GP0 decoding and the GPU layer: 12%.
- The models' snapshot (field_mesh.c): 3.6%.

The rest is the GTE (~7%), the SPU's voice stepping (~7.5%) and the game's own field code (a few %). The earlier
"game logic alone is ~16 ms" no longer holds for the light fields.

Per field frame (two VBlanks) these scenes draw 370-470 background tiles (16x16 sprites, the game re-linking the same
packets every frame) and 12-63 GP0 polygons; the models (about 1,000-1,600 polygons) go through the native path. No
single hot spot: `emit_scene` is 3.6% with fine buckets (reading each recorded primitive), `fm_emit`'s time is
spread over its per-polygon loop, the scattered projected vertices and the stores. The geometry engine never makes
the CPU wait (the chunk DMA wait is under 0.1 ms per VBlank). Two experiments without effect: keeping the command
chunks in the data cache (clean without invalidating), and the residual checks in the tile path.

Halving the rendering needs work that isn't redone every frame: the background tiles (the same packets, scrolled)
recorded, planned and emitted once and replayed while unchanged, as the models' native path does for meshes. A
second lever outside rendering is the SPU's voice stepping on the ARM7 (about 3 ms per VBlank everywhere), at the
cost of up to a frame's latency on the values AKAO reads back (a decision for the user).

### The background composite for still backgrounds (2026-10-26)

The composite (the sprites behind everything in their cells, drawn once into bank A) now also serves a scene whose
background sprites have hashed the same for 4 frames, and stops at the first change (a scrolling scene would
compose every frame: forced on, d3_f2205 went from 31.0 to 41.5 ms). Still scenes: d1_f0110 21.7 -> 19.4 ms per
VBlank, d4_f2110 34.9 -> 33.9, d3_f2205 31.0 -> 31.6 (the per-frame hash). Walking (8 fields of the walking sweep):
the same within 0.5 ms. The composite's colours sit one step of the LCD's 6 bits below the 3D layer's (the 2D engine
widens a 5-bit colour as 2c, the 3D engine as 2c + 1), as for the over-budget scenes before.

### The SPU on the ARM7 (2026-10-26)

`SPU7=1` (the default in DS mode) moves the emulated SPU's voices and the hardware channels to the ARM7 (spu7.h,
port/ds/spu7_client.c, port/arm7/spu7_server.c; the same spu_core/voice/source/adpcm/envelope code, as ARM code in
the ARM7's WRAM: 33.5 KB of code and 17 KB of data of its 64 KB).

Checked on the intro at frame 1800 against the ARM9 path (`SPU7=0`):
- the same key-ons (161) and key-offs (219);
- 8 sample rings in use in both;
- voices 0-2 and 7 up identical, voices 3-6 holding the same notes in another order (AKAO's voice choice follows
  the later readbacks).

Two bugs found that way:
- the client's reset left the control register at 0, so AKAO's read-modify-write turned the SPU off;
- a just-keyed voice read back the last note's state (level 0, ENDX set): AKAO detached it and never keyed it off.

ms per VBlank, before -> after:
- d1_f0110 19.4 -> 16.9 (PS1 16.7).
- d4_f2110 33.9 -> 30.4.
- d3_f2205 31.6 -> 31.3.
- Battles d1_b0004 28.1 -> 27.7, d2_b0300 39.6 -> 35.8.
- The 8-field walking set: 1-4.7 ms faster each, no crash.

The intro's movie now has 10.6 ms of its 16.7 free.

### Battles and the heavy fields (2026-10-26, later)

Measured with the PC sampler windows (PROFILE=3 with PCLO/PCSHIFT): the full-RAM profile's 2 KB buckets misplace
whole functions (port_native_mesh showed 7%; in fact the code around it). To read an overlay area's lines, map the
bucket through the object's own section: `arm-none-eabi-addr2line -j .psxovl_0a.port -e
build/ds/port/ds/gx_hybrid.o <address - the section's start in build/ds/ff9ds.map>`.

What a heavy battle (d1_b0071) spends, about: the native meshes' collection and emission 20%, the GTE 12%, the UI
(software rows, the 2D layer) 15%, the GP0 decoding and recording 10%, the game's own code under 10%. Removing the
3D emission entirely saves 4.1 of 32.8 ms, and writing the command lists to DTCM without DMA 0.55 ms: the emission
costs its computation, not its stores.

Changes, each with identical frames:
- emit_scene finds the OT entries with native polygons through a bitmap.
- Opaque textured native polygons (most of a battle's) are emitted straight from their meshes (native_run, ARM in
  ovl_0b's area), without a Prim record.
- port_native_mesh in ARM in ovl_0b's area; NCLIP factored; texel bounds gathered per use in registers.
- regions_touch passes over writes away from every region and palette (a 32x32 cell bitmap).
- dirty_cells sets a cell row's bits at once.
- The GTE's MVMVA (and lighting steps) and GPF in 32 bits where that is exact (checked against the 64-bit formula on
  2,000,000 random cases).
- The overlays' code is built with PORT_GTE_APPROX (decomp-config tracks DSMODE), and the fast rtir/rtv0tr/rtv0 and
  gpf12 paths are calls rather than inline bodies: smaller overlays (heap +2.4 KB) and faster.
- MoveImage, LoadImage and StoreImage copy whole rows when no mask bit or wrap applies; ClearOTagR links four
  entries a step.
- Fields: the model snapshot in ARM with register-gathered bounds; each group's screen bounds kept with its
  snapshot for bgc_use (in a table beside the stores: growing FGroup took from the stores' vertex room, and
  d4_f2110's groups then fell back to packets).

Tried without gain: the native polygons emitted in mesh order after the walk (cache-friendlier, no faster, and the
frames differ); the battle's clearing fill limited to touched cells (0.07 ms); the batched RTPT in two passes (slower).

ms per VBlank, before -> after: d1_b0071 34.5 -> 31.4, d2_b0300 35.8 -> 30.6, d1_b0254 36.6 -> 33.8, d4_b0325
31.0 -> 27.8, d2_b0128 34.5 -> 30.2; d4_f2110 30.3 -> 27.4, d2_f1102 23.5 -> 23.1, d1_f0110 16.9 -> 16.7.

### World maps, the other modes (2026-10-26, later still)

World maps (original DS, ms per VBlank, before -> after): d1_w9009 35.6 -> 31.6, d3_w9006 40.5 -> 34.9, d4_w9006
36.5 -> 32.0. What did it:
- The GTE's depth cue (DPCS, DPCT, INTPL, the NCDS/CDP step) in 32 bits where that is exact: about 1,100 a VBlank.
- The block pass (port_native_world_block) and the terrain's emission (terrain_emit) in ARM in ovl_0c's area; the
  texture use cached across faces, its bounds gathered in registers; each use's state and colour scale once a frame
  instead of a division a face.
- The vertex pass in port code (port/game/world_verts.c): of the two RTPTs a group goes through, the first is read
  only for its depths (the second overwrites everything else first), so it pushes the depths alone.

The other modes, entered through the scenario layer's new `field_exit=4|7|9` (`tetra=1` for 9): a field left at
once for the ending, game over or a card game.
- Tetra Master crashed on entry: its image file buffer is named by its PS1 address and used as a number
  (TM_IMAGE_FILE, TM_PRIM_BUF; decomp request F34). With walking input a game now returns to the field (host);
  card selection runs at 61.6 ms per VBlank on an original DS (software).
- The ending (from disc 4: its data is there) runs on the host for 9,000 frames, and on the DS at 16.7 ms per VBlank
  while its movie plays.
- Game over: its picture's fade is a full-screen semi-transparent tile every frame, which went through plot() a
  pixel at a time: 258 ms per VBlank, now 56.6 (tile_semi, two pixels a word).

Exact additive layer: its triangles skip the rows the 5:4 scaling never samples (every fifth): d3_f1812 61.3 ->
59.7. Approximating that field's sunlight in 3D instead (a lower area limit) darkened its upper half: not taken.
The background composite redraws only the 8-row bands whose sprites changed.

ITCM is full: tile_semi, psx_gte_rtpt_depths and the RGB24 display scaler (the MDEC fallback for 24-bit movies)
live in main RAM to make room.

Crash sweep, 40 more fields (`build/validation/fields40.txt`, walking input from frame 300, original DS): 40/40
reach frame 1500, 17.0-76.4 ms per VBlank (median about 28); outliers d1_f0107 76.4 and d3_f2401 61.3.

### Against the software renderer (2026-10-26, evening)

The hybrid's screenshots had only been compared with earlier hybrid builds. The all-software DS build (the same
scenario and frame, `FLAGS="HWSOUND=1"` in a copy of the bench script) is the reference: its rasterizer matches
DuckStation (render-colour-validation.md), and the game state at a frame doesn't depend on speed. Compare after a
6-pixel box blur: the 5:4 scaling's phase differs between the 3D scene and the software display's scaler, which
leaves edges different in every field without either being wrong.

Found and fixed:
- The additive layer cleared only its last band of bank A: rows an earlier software frame or the composite had
  written were added over the scene (Alexandria and d3_f1812 washed out by a ghost of an old frame, +70 to +95 per
  channel). Now everything outside the band is cleared when something else wrote bank A.
- Textured vertex colours (TEX_TINT): c x 31 / 128 rounded and one step under, against the 3D engine's rounding up
  of (T + 1)(V + 1): bias d1_f0453 +4.7 -> +2.5, d1_b0071 +10 -> +6 per channel.

Left: blurred mean difference under 2 in 17 of 28 fields, 4-8 in the rest; the battle 7.8 (slope 0.95, +10 in the
darks: the 3D engine's modulation lifts dark texels by up to a 6-bit step). Models shade more smoothly than the
PS1's dithered gouraud. The battle's 2D layer differs (the selected row's highlight, a black line over the panel):
the top-screen UI, which the dual-screen redesign replaces.

Performance (original DS, ms per VBlank): the additive layer threw away bank C's background composite whenever it
ran, so Alexandria recomposed its background every frame: d1_f0107 74.2 -> 55.8; with fill_buffer in STMIA bursts
and semi4's loop for 4-bit dithered semi polygons (Alexandria's 132 lights, 84% of their texels transparent) up to
area 1024: d1_f0107 79.5 -> 53.6, d3_f1812 59.7 -> 55.4, d3_f2055 41.2 -> 38.1. Not taken: a cache of decoded texel
rectangles (melonDS doesn't model the data cache, so the copies only cost); emit_scene in ARM (55.4 -> 55.0, 5 KB).

A still background under additive primitives is composited too (bank C, held 60 frames after the last one; before,
a single additive primitive kept its tiles in the 3D scene). The 28-field sweep after it (original DS, ms per
VBlank): d1_f0205 23.7 -> 19.5, d2_f1203 18.9 -> 16.7, d2_f1425 24.1 -> 21.3, d3_f2055 41.2 -> 38.4, d1_f0453
31.0 -> 29.5, the others within 1; every field as close to the software renderer as before or closer (blurred mean
difference under 1.5 in 15 of 28, 4-7 in the rest, d3_f1812 57 -> 4.8 with the bank A fix). Eleven of the 28 run
at the PS1's 16.7.

The composite is drawn again only for VRAM writes to what its sprites read (per texture page the rows, and the CLUTs
themselves, taken when it is drawn): d3_f2055's animated CLUT elsewhere in the CLUT area recomposed everything each
frame, 38.4 -> 18.4 ms per VBlank. Where it stands for the heaviest fields: five of the seven at 27-30 ms are
composited already (the time is the field models, fm_emit and port_native_field_mesh about 20%, and the game);
d2_f0955 (28 of 32 bands change every frame: the camera moves) and d4_f1706 (11) aren't still, so their tiles go to
the 3D engine every frame. A scrolling background needs the layer composites of design §5.3 (each layer once, then
scrolled), not yet built.

### DS coverage, 100 more fields (2026-10-26, night)

100 fields no DS sweep had run (random from the 747 left, frame 1500, original DS): 100/100 reach frame 1500
without a crash or stall; median 23.3 ms per VBlank, 16.6-112.4. The outliers are scenes past the 3D engine's
RAM, drawn wholly in software: d1_f0574 (Lindblum's festival, two close-up NPCs) 112.4, d4_f2907 87.3, d2_f1662
83.2, d3_f2000 65.8. d1_f0574 has 1,272 recorded polygons (459 background sprites) and 1,557 native model
polygons: 2,829 polygons and 9,189 vertices a frame against 2,048 and 6,144; without the background sprites
still 7,497 vertices, and only 5 of the other 813 primitives are behind everything else in their cells. Such a
scene needs two 3D passes per frame (two DS frames per 30 fps game frame, the first captured) or dropped geometry:
not built.

100 more (the next of the shuffled 747): 100/100 reach frame 1500 without a crash or stall; median 24.4 ms per
VBlank, 16.6-105.7 (d2_f1225 105.7, d4_f2955 67.0, d4_f2926 55.6, d3_f1866 53.2). No screen of the 200 blank or
flat. 584 of the 1,131 fields have now run on an original DS. The status screen's sound line read a stub: with
SPU7 audio_hw.c isn't linked and the stub generator made `port_hw_sound_stats` a 12-byte function; the line is
gone from SPU7 builds.

200 more: 199 reach frame 1500 without a crash or stall (median 24.2 ms per VBlank, 16.5-110.0: d4_f2902 110.0,
d2_f1751 108.9, d4_f2915 93.0, d2_f0957 65.9); d2_f0070 stops at frame 123, the fixture limit the host sweep
found (the opening scene, whose FMV001 is on disc 1). The near-black screens are narration (d2_f1461, d3_f1819).
783 of the 1,131 fields have now run on an original DS.

The last 347: 347/347 reach frame 1500 without a crash or stall (median 24.7 ms per VBlank, 16.4-107.5: d1_f0576
107.5, d1_f0851 and d2_f0851 71.6, d3_f2955 67.0, d2_f1660 64.8); the near-black screens are a minigame's retry
prompt (d3_f2204) and a starfield (d4_f2921). Every field ID on the four discs (1,131) has now been entered on an
original DS and run its first 1,500 frames: the only stop is d2_f0070, the fixture limit. Entry and the first 25
seconds only: events later in a field need their own scenarios.

Battles: 200 random encounters of the 1,085 disc/encounter pairs that start on their disc
(`build/validation/battlesweep11`), none run on the DS before: 200/200 reach frame 1500 without a crash or stall,
no screen blank; median 26.8 ms per VBlank, 18.6-64.5 (d2_b0038 and d4_b0038 64.5, d3_b0002 61.1, d4_b0931 59.1).
300 more: 300/300 reach frame 1500 without a crash or stall, no screen blank; median 27.2 ms per VBlank, 18.3-77.5
(d3_b0057 and d4_b0211 77.5, d4_b0935 68.0, d4_b0168 60.4).
The other 536: 536/536 reach frame 1500 without a crash or stall, no screen blank; median 26.9 ms per VBlank,
17.2-77.5 (d3_b0211 and d4_b0057 77.5, d3_b0890 77.4, d3_b0851 68.2). With the 49 earlier sweeps had run, every one
of the 1,085 disc/encounter pairs that start on their disc has now run its first 1,500 frames on an original DS
(1,036 of them on this build): none crashes or stalls. Across the 1,036, the median is 26.9 ms per VBlank against
the PS1's 16.7, and 4 in 5 run between 23.5 and 35.8.

### Wandering the fields on the host (2026-10-27)

Every field (`build/validation/fieldsweep/cfg`, 1,131) on the host for 20,000 frames with scripted play: a direction
held 180 of every 200 frames (up, right, down, left in turn) and confirm (CROSS) every 30 frames, so each run walks
into neighbouring fields, talks, opens shops and fights its random encounters (3-30 s a field, no rasterising).
1,085 pass. The failures:
- 26 fixture limits: walking into a field the fixture's disc doesn't have ("unavailable field ... in current
  archive": story state the direct entry doesn't set), and three reads of sector -1 (d2_f0070, d3_f1609,
  d3_f1800: a file absent from that disc).
- 17 crashes, fixed (config/patches.txt, PSX_NZ): script opcodes 0x95/0x96 and the 0x2F store with an object id
  that names no object, and btl_rollAmbush reading an empty party slot after a party change; all 17 now run their
  20,000 frames.
- Open, both the PS1's own out-of-bounds behaviour that its memory map survives:
  - d2_f1750 (fixed 2026-10-27): fldmdl_drawMesh links a polygon into an OT entry past the table (z = depth sum
    >> 4, unclamped in the original asm too); the link word (0x00147890, a KUSEG packet address) lands in another
    ModelObj's model pointer, which the PS1 then reads through the KUSEG mirror and the port can't
    (fldmdl_calcBoneMatrices faults). The port now leaves such a packet unlinked (PSX_OT_OUTSIDE, psx_mem.h; a
    counted patch in model_link.c and the same test in field_mesh.c's link_translucent): the PS1 never draws it
    either (the walk doesn't reach past the table). The wander run: a crash at frame ~2000 before, 20,000 frames
    after; host frames of d4_f2110, d2_f0955, d1_f0453 identical with and without it.
  - d1_f0310 (guarded 2026-10-27): after a battle, a sound effect channel (18) keeps reading its sequence at
    0x8010AE93 in the overlay area the field overlay has replaced; opcode 0xFE 0x4F indexes g_akaoOpsFE (at
    0x8006FCE0, 0x20 handlers) far past its end. The exe's word there is 0x102D031D, data, an odd address: the PS1
    would take an address error too, not run another handler. The port sends a 0xFE opcode past the table to the
    unused slots' handler (akao_opInvalid: the track ends; a counted patch in akao.c). Not reproduced since the
    first sweep (the second sweep's run of d1_f0310 passes with and without the guard).
  - The world map's wm_drawMesh links with the same unclamped index (the decomp's note, F35): the same guard
    (world frames identical with and without it).
  Reproduce: `tools/docker.sh gdb --args build/pc/ff9ds --scenario build/validation/fieldsweep/cfg/<field>.cfg
  --frames 20000` with the wander presses (any 200-frame direction cycle plus CROSS every 30 frames from 400).

The battles the same way (`build/validation/battlesweep11/cfg`, 1,085 encounters, 20,000 frames with confirm every
30 frames from 300: each fight's commands until it ends and returns to its field): 1,082 pass, no crash. The other
three are fixture limits: the battle returns to a field the fixture's disc doesn't have (d2_b0076 to 2054, d4_b0246
and d4_b0878 to 2500).

### The slowest fields, by cause (2026-10-27)

- Past the 3D engine's polygon or vertex RAM (gxh_stats_soft 8): d1_f0574 (2,829 polygons, 9,189 vertices), d2_f1225
  (with additive primitives, 520). Wholly software, 105-112 ms. The user's decision: no dropped polygons;
  simplified replacement models only as a last resort. The route is two 3D passes a 30 fps game frame (the far
  primitives in the first DS frame, captured, then shown behind the near ones in the second). d1_f0574's textures
  need 125 of the 128 2 KB units the two banks left by the captures would hold.
- Textures that don't fit banks B-D (gxh_stats_soft 128): d4_f2902, 110 ms. Two passes don't help it by themselves.
- d4_f2907 (rain, hybrid): some 260 shaded semi-transparent lines a frame, 217 pixels each, divided five times a
  pixel; stepped instead: 87.2 -> 79.4 ms. plot() inline for those pixels gained nothing (80.4).

### Two 3D passes for scenes past the 3D engine's RAM (2026-10-27)

A field scene past 2,044 polygons or 6,140 vertices (after the composite) is drawn in two passes instead of in
software (gx_hybrid.c, tp_*). The split is a depth (the recorded primitives' OT slots and the models' vertex depths
share a scale; larger is nearer): pass one the polygons farther than it, pass two the rest, the split chosen from a
256-bucket histogram so that both fit and the vertices divide evenly. Banks: A is the first pass's capture (LCD) and
then BG3 behind the second pass (its clear transparent, alpha by GFX_CLEAR_COLOR in the VBlank interrupt); B holds
the whole screen, captured at the second pass and shown (VRAM display) while the next first pass renders; textures
in C and D only (128 units). The interrupt (gxh_vblank) switches them when each list has swapped. The second pass is
emitted right after the first swaps, before the game goes on: the models' packets change with the next frame
(texture animation), and the passes must see the same (emitted later, 577 model polygons a frame lost their
texture). Without the composite and the additive layer (their primitives in 3D); a scene whose textures don't fit
C and D stays in software.

d1_f0574 112.4 -> 83.6 ms per VBlank (blurred difference from the software frame 9.8: the display is a frame late),
d1_f0576 107.5 -> 66.7. Not helped: d2_f1225 (textures past C and D), d4_f2902 (textures past B-D: no two-pass
needed by polygons), and the slow fields that are slow for other reasons. 5,000 frames of walking from d1_f0574
on the DS leave the mode for the next field without a fault.
The split from one pass over the polygons (the depth range from bounds: slots and each model group's far and near
depths) and the second pass after the rest of the present (the UI layer's work overlaps the first's swap):
d1_f0574 -> 79.6, d1_f0576 -> 58.9 ms per VBlank, frames identical.

Textures that don't fit (software still): d4_f2902 places 118 of 255 units with bank A free, yet a background run
and a few small converted (subtractive) groups fail each frame: the background runs' rows, not the units, are what
run out (to be looked into). d2_f1225 fits B-D with its composite and additive layer (104 units) but not C-D alone
when it is past the 3D engine's RAM, so its two-pass frames go software.

### Loads in PS1 time: tried, not taken yet (2026-10-27)

A loader's poll delivered a sector at once, so a load took no virtual time: no VBlank, no AKAO tick passed through
it (sound stops during loads, and d1_f0310's battle sound effect still played from its data when the field's
overwrote it). Letting a run of 512 polls stand for a frame (as VSync(-1) busy-waits already do) and the sectors
come at the drive's rate fixed d1_f0310 on the host, and the host wander sweeps found three more problems through the
new timing (fixed: below), but every DS field load then crashed: frames now pass while a field streams data, and
the DS's native field models (port_native_field_mesh, a stub on the host) read groups half overwritten, writing
through a NULL pointer into the ITCM's mirror at 0. Reverted, then found: not the field models themselves but the DS's stack. A frame a loader's poll lets pass ran the
whole present (gx_hybrid's scene and UI work) on top of the game's own calls deep in the load, which overran the
DS's DTCM stack and corrupted what lay below (a call through a NULL pointer, garbage, the ITCM). Such a frame now
keeps time only (psx_poll_frame: no new picture, as the PS1's screen holds its last one through a load), with AKAO,
the VSync callback and the drive going on. Taken: d1_f0310 runs on the DS past its old stop (frame 12,048, now
12,966 and on), the 28-field sweep runs as before (frames at 1500 differ a little: the loads took the PS1's time),
and music and sound now go on through loads.

Found through it and kept:
- Effects with two modules: the second loads at 0x801EC700 (btlfx_pool.c, d->unk10; the _1 builds). Calls into it
  had no native function (only the first slot was identified). Now each slot is identified on its own; on the DS
  the _1 modules link at the top 12 KB of psx_fx_area (FX_SLOT2_SIZE), the others at its bottom (at most 14.3 KB).
- NULL reads: btl_clearCmdQueue with no current command (d1_f0761), fx_0fb_1's rocks testing a target not yet set.

d4_f2902's software frames: its background needs (286 page rows of blocks) took more than the 512 texture runs.
A one-block gap in a row's blocks is now bridged (one run, a block more of texels): it fits, 115 -> 35.9 ms per
VBlank; six other fields' frames identical. Its subtractive (PS1 mode 2) layers are now the 3D engine's
approximation (black at the source's brightness), as in every other hybrid field: lighter than the software frame
(blurred difference 8.9). The DS has no exact subtraction in either engine; exact means software (115 ms here).
d2_f1225 in two passes: 126 units placed and 175 groups and runs left over in C and D, its background sprites all
in 3D (the composite, which takes them out of the texture plan at 104 units, needs bank A or C, and the passes hold
A and B). A composite with two passes needs a third bitmap bank, leaving D alone (128 KB) for the textures: not
built; the scene stays in software (105 ms per VBlank).

d2_f0931 (56 ms per VBlank; at frame 1500 now a narration screen over a scene still drawn behind): the composite is
kept for being past the 3D engine's RAM, and 27 of its 32 bands change at most redraws (416 of 589 frames), so it is
drawn again almost whole: 52 ms a redraw (fill 1.6, replay of 717 background sprites over 175,554 pixels 42.6, scale
7.1), plus 32 ms of emission a frame. The sprites don't scroll (matched by texture source, nearly all stay put):
the band hashes change with the sprite list. Tried, not taken: two passes instead of the composite for a moving
background (no gain here: its textures don't fit C-D; Alexandria 53.6 -> 68.8, losing its composite and additive
layer); the redraw's scaling in ARM (no gain: melonDS charges ARM fetches from main RAM as Thumb's).
Tried, not taken: moving background layers (a slot whose sprites changed in two of the last four frames) kept out of
the composite and its stillness: d4_f2110 27.9 -> 30.9, d2_f0955 34.3 -> 35.6, d4_f1706 34.1 -> 32.5 but its frame
changed (blurred 4.4).

What the moving-camera fields change, layer by layer (each OT slot's background sprites against the last scene's):
d2_f0955 changes nearly every layer as a set each frame (tiles entering and leaving: a scrolling camera); d4_f1706
keeps its layers but for about 10 animated sprites a frame; d2_f0931 has still, uniformly shifted and changed
layers. Composing scrolling layers apart (a full-screen 16-bit buffer each, about 150 KB) doesn't fit the DS's
memory, so such fields keep their tiles in 3D; d2_f0955's frame is then mostly its models (fm_emit and
port_native_field_mesh 21%) and the tiles' record, plan and emission (15%), all already in tight ARM loops.

### What melonDS measures (2026-10-27)

`tests/cache/cache_bench.c` (`make build/tests/cache_bench.nds`, run with tools/ds_profile.py --frame 100): the same
262,144 word loads from a 4 KB array (cache-resident on a DS), a 1 MB array read through (a line fill every 8 loads)
and a 1 MB array a 32-byte line a load (a miss each on a DS): 11.0, 9.0 and 12.0 cycles a load, loop included, on the
original-DS setting. melonDS doesn't model the ARM9's data cache: every load costs about as much, cached or not.
Every ms-per-VBlank figure here is that machine's: on hardware, cache-friendly loops (most of the port's) should run
faster than measured and scattered accesses (texels, PS1 VRAM lookups) slower. Only a real DS settles parity.
Calibration tried with melonDS PR 2235's accurate-timing interpreter (github.com/Jaklyy/melonDS, branch chemical-x2,
built at ~/src/melonDS-accurate; tools/ds_profile.py runs it with MELON_BIN=<binary>, config in ~/.config/melonDS,
JIT off): it does model the cache (cache_bench: 6.2, 11.0 and 47.1 cycles a load for the three patterns), but whole
scenes don't measure consistently: d1_f0110 85.2 ms per VBlank with PROFILE=2 (reproducibly), 23.9 with PROFILE=3
(its sampler's 8 kHz timer interrupt on), d4_f2110 100.1, d1_b0071 96.5. Something in its halt or interrupt timing
is off: not usable for calibration as it stands.

### The field models' passes, per polygon (2026-10-27)

With fine buckets (PCLO/PCSHIFT=4), d4_f2110's model pipeline is about 46% of its sampled time: the snapshot
(`port_native_field_mesh`, 16.5%), the emit (`fm_emit`, 16.6%) and the GTE's projection and bone matrices (~13%).
Battles are the same shape (d2_b0300: `port_native_mesh` 10.1%, `native_run` 9.7%, `emit_scene` 9.7%, GTE ~8%). The
hot lines are the first loads of each polygon's mesh record and packet; the emit's per-vertex loop spilled about nine
values to the stack per vertex. Three steps, ms per VBlank (d4_f2110, d2_f0955, d1_f0453):
- fm_kind: the indices loaded once as two words, its state in locals (27.86, 34.24, 29.65 -> 27.82, 34.19, 29.62;
  identical frames).
- The snapshot: the front bits a word at a time in a register, the indices once (-> 27.42, 33.83, 29.38; identical).
- Each group placed by a position matrix (the drawing offset, its depth and 4 per entry: the snapshot stores x and y
  plus 1024 in 11 bits, so a vertex is two masks and a shift; a group whose depths leave +-32000 keeps its packets
  instead of being clamped) -> 26.95, 33.41, 29.08. The 3D engine's clip matrix (position x projection) rounds:
  49-95 scattered pixels a frame differ, edges a pixel over (the approximate geometry of 2026-10-25).
- Battles: native_run keeps the last texture use's words (region, image, palette, origin; it recomputed them for every
  polygon) and its drawing state in locals: d2_b0300 28.94 -> 28.72, d1_b0071 26.59 -> 26.44, d2_b0128 29.03 -> 28.69
  (identical frames; the heap 112 bytes smaller).
- Battles: each model polygon's OT entry is worked out in the snapshot (port_native_mesh, which has its indices and
  depths in hand) and kept per polygon, as a background record's are, instead of collect_native reading the mesh
  records and per-vertex depths again: -> 28.32, 26.09, 28.29 (identical frames).
- Battles: native_run's positions as one add of the packed drawing offset (the GTE's SXY already saturate to 11 bits;
  a background record's slots, which may be an earlier record's, are made so as they're read) and its walk state in
  registers: -> 28.11, 25.96, 28.04 (identical frames).
- port_native_mesh's uv table and size-tier counts in locals: -> 28.08, 25.93, 28.01 (within noise; identical frames).
  The per-polygon loops are now load-bound; what's left of the model pipeline's cost is the two passes themselves.
- With fine buckets over the ITCM, d4_f2110's projection (`gte_rtpt_batch`, ~3,400 vertices a game frame) is 10.4% of
  its samples (the 2 KB buckets had spread it over its neighbours). Its saturations as one range check per pair,
  done in branches (an empty asm keeps GCC from selecting both clamps every vertex): 112 -> 81 instructions a vertex,
  d4_f2110 26.95 -> 26.79, d2_f0955 33.41 -> 33.22, d1_f0453 29.08 -> 28.94 (identical frames).

### Where a field's background goes (2026-10-27)

Timers (PROFILE=1, with section timers added for the measurement) on d4_f2110, ms per VBlank of 27.0: the models'
projection 3.07, snapshot 4.95 and emit 4.96; the OT walk 4.8 (2.4 of it the sprites' recording, hybrid_rect, about
200 a VBlank); field_frame 2.1 (bgc_hash 0.35, bgc_use 0.62, plan_textures 0.82); the rest of emit_scene 2.5. A
build with the background overlays unlinked (a patch on fldbg_drawOverlay's addPrim, measurement only): d4_f2110
26.8 -> 20.2, d2_f0955 33.3 -> 23.8, d1_f0453 29.2 -> 18.4 ms; with them walked and decoded but not recorded
(gp0_tpage_sprite16 returning before the backend): 21.5, 24.8, 19.6. So the walk and GP0 decoding of the tiles are
about 1.1 ms; the renderer's own handling of them (record, the composite's hash and choice, planning, emit) 5-9 ms.
d4_f2110 and d1_f0453 hold a still composite (302 and 280 of their 468 and 503 primitives in it, recorded, hashed
and planned every frame though the composite doesn't change); d2_f0955's background moves (no composite: 423 of its
456 primitives in the 3D scene).

First steps on the per-sprite cost: target() checks a version the game's PutDrawEnv/PutDispEnv bump before its
lookups (two calls and nine compares a primitive), and field_sprite makes its record's twelve words in registers.
d4_f2110 26.79 -> 26.39, d2_f0955 33.22 -> 32.91, d1_f0453 28.94 -> 28.60, d2_b0300 28.08 -> 27.64 (identical
frames). The larger step is not recording a still composite's sprites at all.
- emit_scene's opaque-sprite path reads a sprite's record as its words (corners and texel edges from x0 x1, y0 y1,
  u0 u1, v0 v1; any other layout vertex by vertex as before): d4_f2110 26.33, d2_f0955 32.69, d1_f0453 28.48
  (identical frames). d2_f0955's moving background: emit_scene outside the models 4.8 ms a VBlank for ~420 sprites,
  plan_textures 1.9.

### Next: the field background by layer, not by sprite (plan, 2026-10-27)

What the measurements above say: a field's background costs the renderer 5-10 ms a VBlank (a quarter to a third of
a heavy field), spread evenly over its per-sprite path (record ~800 cycles a sprite, the composite's hash and choice,
planning, emit ~760 cycles a sprite), with no hot spot left to tune. The game draws each background layer
(fldbg_drawOverlay, ovl_0a) as the same prebuilt sprites at fixed offsets and fixed OT entries (the layer's depth plus
each tile's), moved together by the layer's position (and, for the looping kinds, wrapped at the layer's width or
height). So the work can be per layer:
1. A port fldbg_drawOverlay (link-time override) that, when the hybrid takes the scene, positions the sprites as the
   game does (their x0, y0 stay as the PS1 has them) but links one marker per (layer, OT entry) instead of each
   sprite, so the walk's order between the background and everything else is unchanged.
2. Per layer, kept while its sprites' words (all but x0, y0) and its texture placement are unchanged: its tiles'
   textures planned once, and its 3D commands built once (positions relative to the layer, depth per entry), sent
   each frame after a translation (the GX position matrix, as the field models' groups). Looping layers: the wrapped
   tiles as a second translation of the same list, or rebuilt when the wrap moves.
3. The composite (still backgrounds, scenes past the 3D engine's polygons): by layer (a layer behind everything in its
   cells), its hash per layer.
Out of the measurement: the d2_f0955 kind (a moving background, 423 sprites a frame) would gain the most (~9 ms a
VBlank); still scenes ~5. Budget for the lists: the sprites no longer recorded free most of the primitive buffers
(ovl_0a's range), where the lists would live.

What the first step found (branch bg-layers): wrapping fldbg_draw (config/renames.txt) and lifting each
layer's run of tiles at an OT entry into a port marker (gpu.c walks markers in place; exact, identical frames) gains
nothing as such, because the still fields put nearly every tile at its own OT entry (d4_f2110: 396 tiles over more
than 64 entries; d2_f0955's moving layers: 410 tiles in 3 runs). Grouping by layer instead (one unit, tiles at their
own depths) is exact for the depth-tested 3D scene, but not for a scene that goes software after the walk
(field_frame's replay draws the records in their order, as a painter): a unit recorded out of the walk's order
would paint wrong. So a layer unit needs either the scene known to stay in 3D at the walk, or a software fallback
that walks the OT again. Its tables also cost the heap 4 KB as static data: they belong in ovl_0a's range.
Tried without gain: an inner loop for consecutive opaque sprites in emit_scene (the per-sprite cost isn't its loop
control), indexing translucent_id's recent primitives by signature bit.

A hardware calibration build (2026-10-27): PROFILE=2 builds print each published block's ms per VBlank on the
status screen (platform.c), so the same ROM can be timed on a DS and in melonDS (melonDS, this ROM: d1_f0110 16.71,
d1_f0453 28.47, d2_f0955 32.94, d4_f2110 26.34, d2_b0300 25.42).

### The host wander sweep again (2026-10-27, evening)

All 1,239 fixtures of build/validation/fieldsweep/cfg (fields, plus the battles and world maps kept there) on the
host for 20,000 frames with the wander presses: no crash of the port's. The failures are fixture limits: fields
and files the fixture's disc doesn't have, sectors at -1, and five runs (d3_f2750, d4_b0137, d4_b0664, d4_b0686,
d4_boss_tiamat) that walked onto the world map in a story state whose world data file isn't on that disc
(wm_loadWorldData read through file_find's NULL). These now stop with a message (a counted patch in wm_main.c), as
the other fixture limits do. d3_f2750 passed the earlier sweep: loads in PS1 time change where the wander goes.

### Background runs (branch bg-layers, 2026-10-27; merged 2026-10-28)

A moving layer's run of tiles at one OT entry (port/game/field_bg.c lifts it into a marker; gx_hybrid.c gxh_bg_run
takes it whole) is not recorded tile by tile: its tiles' positions and packets are snapshotted at the walk (the game
rewrites them and their links for the frame after next, which can come before the scene is sent: read later, the
layer drew a pixel off), planned into the tables after the primitives, emitted between the opaque and translucent
primitives (from the walk's last, as the opaque primitives go), and replayed at their place in the walk for software
scenes. Only for layers that moved in the last 8 frames (a still layer's tiles stay recorded, for the composite).
Its code is ovl_0a's, its state at the end of the buffer's primitive room (the heap: -0.8 KB). The 28-field set
against master: d2_f0955 32.79 -> 30.52, d2_f1057 24.57 -> 22.49 (both moving backgrounds), most others 0.1-0.5 ms
slower, and two problems: d2_f0931 54.1 -> 114.7 (a scene past the 3D engine's polygons that lives on its
composite: its moving layers' tiles, now quads of their own, take it past the limit), and d4_f1911 darker than
master's frame (not yet understood). Kept on the branch.
Follow-up on the branch: d4_f1911's darker frame was a still layer taken as runs (out of the composite: its tiles
then drawn by the 3D engine, a few pixels from the composite's software drawing, which matches the PS1: blurred
difference 0.66 against 5.87). Telling still from moving by the layer's fields or a sample of its sprites' positions
failed (the game's two halves of a layer's sprites keep culled tiles' old positions); by each run's own positions
(hashed at the walk, per buffer) works: d4_f1911 identical. But still runs then cost the lift, the check and the
sprites' path (still scenes 0.9-1.3 ms slower than master), and d2_f0931 stays at 115 ms with no runs at frame 1500.
Net: about 2 ms gained in the two moving-background fixtures, lost across the still ones. Not merged; the branch
keeps it.
Merged 2026-10-28 with three gates, frames identical to master in the 8-field set and two battles:
- runs only in a field whose scenes keep well inside the 3D engine's polygons (off at a field's start, on after 8
  scenes under 1,500, off for the field past 1,700 or once the composite holds a scene: d2_f0931 115 -> 54.6 ms,
  master 54.1);
- a run found still is left linked by field_bg.c (a per-buffer list cleared every 32 frames), and nothing is lifted
  while runs are off;
- the buffers' primitive capacity is kept per buffer (bg_runs_set), not computed per recorded sprite.
d2_f0955 32.79 -> 30.52 (a moving camera, the walking case that was slowest on hardware); the still fields stay
0.1-0.8 ms over master (d4_f0351 +0.8 with no runs taken). Builds that differ only in where code lies differ by a
few tenths of a ms in melonDS (the same work: the no-lift build 25.85, the gated 26.11), so differences under ~0.5
ms between two builds are not measured effects.

### Field models through the 3D engine (branch gx-models, 2026-10-28): not working yet

The user's choice of 2026-10-27 allows approximate depth for field models, for the 3D engine to transform them. On
the branch: fldmdl_projectMesh keeps the bones' matrices (RT x bone, through the GTE as before) and defers the vertex
pass (fm_flush_projection runs it for whatever reads g_vtxScreenXY: the packets' path, the mirrored draw); the
renderer takes a group from the matrices (each vertex's view Z for its OT depth, the PS1's culling as the sign of the
view-space determinant, the texture uses, the screen bounds from the view box) and emits it with a projection whose
depth matches the OT scale at the group's nearest and farthest vertex (z = a + b / Z), the bones on the matrix stack.
Measured: emitting every polygon (no culling) costs d4_f2110 only ~0.9 ms, so culling in hardware would have been
affordable. What stopped it: groups with more than one bone draw as garbage (not from restoring matrices inside a
polygon: one matrix a polygon is as wrong); every scene got slower (d4_f2110 26.4 -> 42.5, d2_f0955 32.8 -> 46.2:
groups declined after the work, Thumb 64-bit products); and ovl_0a's native area grew 8 KB: with less than a buffer
of room at slot 0's end, the second buffer stays in ovl_0a's MIPS range and the models' ring no longer fits (no
native models at all: fields in software, 90 ms). Any growth of ovl_0a's native code (debug included) does that
silently; worth an assert (gen_link.py: slot 0's end less ovl_0a's native size >= OVL0A_FIELD_PRIMS x 48).
Follow-up (2026-10-28): with the field scene buffer threshold lowered to 1,200 primitives on the branch (room for
debugging in ovl_0a), the group's projection read back sane (d4_f2110's first group: H 401, the view height 224, Z
2,916-3,182, z = -0.345 - 283 / Z on the 3D engine's scale), and the orthographic projection the 3D engine holds
(read back from the clip matrix) has the layout assumed (translation in the last row, 4096 = 1.0). Vertices given as
their CPU view coordinates under one scale matrix (no bone matrices) still draw as huge garbage: the perspective
projection itself, or how the 3D engine takes it, is what's wrong. Not taken further.

### What the background costs, and the composite under animated tiles (2026-10-28)

Upper bounds, ms per VBlank: with fldbg_draw not called at all (no background), d4_f2110 26.5 -> 19.8, d4_f0351
26.1 -> 17.5, d2_f0955 30.5 -> 21.8; with the game building and linking its tiles but the tiles unlinked before the
walk (field_bg.c), 20.7, 16.7 (PS1 speed) and 24.1. So the game's own tile work is under 1 ms; the port's handling of
400-480 tiles a scene is 6-9 ms, about 1,000 cycles a tile. Most of the still fields put one or two tiles at each OT
entry (tiles at the heads of entries, d4_f0351: 335, none in runs of 4 or more; d4_f2110: 483, 120 in runs of 4 or
more; d1_f0453: 470, 280; d2_f0955: 410, all in 3 runs), so the background runs (field_bg.c's markers) can't take a
still field's tiles.

d4_f0351's frame went to the 3D engine tile by tile: its fire (an animated layer, 9 of the 32 bands every fourth
scene) reset the composite's stillness count, so the composite never held. The count now takes a scene as still when
at most half its bands changed (the composite redraws the changed rows alone; a moving camera changes all of them):
d4_f0351 26.1 -> 24.8, and its frame much nearer the software reference (blurred difference 8.17 -> 1.51: the
composite draws the tiles as the PS1 does, the 3D engine samples them). The other seven of the 8-field set are
unchanged, frames identical.

The same scene's additive glow is approximated in 3D (its area past a sixth of the screen), from an A5I3 copy whose 8
colours conv_choose picks from the texture's CLUT each time the texture is loaded (once a scene: the CLUT animates,
so a cache of choices never hits). Its candidate dedupe was a linear search per CLUT entry and its code a second
nearest-colour pass; now a hash in conv_scratch and the farthest-point search's own nearest: d4_f0351 24.85 -> 23.07,
frames identical.

With the composite holding, d4_f0351 (23.1) still spends ~6 ms on its tiles: recording (hybrid_rect and the GP0
path, ~7% of its busy time), the composite's per-primitive passes (bgc_use, sources, row recomposes for the fire),
the emit of the tiles in front of a model, and the planning. d4_f2110's frame is mostly its models: 9 groups, 3,334
vertices and 2,714 polygons a scene (1,168 facing the camera), port_native_field_mesh 16.6% and fm_emit 15.5% of its
busy time in a steady window (tools: build/validation/p3_window.sh, two states of one PROFILE=3 ROM subtracted).

### Exact speed steps (2026-10-28, afternoon)

All with frames identical to the previous step (8-field set, the world map, two battles). ms per VBlank:

| step | d4_f2110 | d4_f0351 | d1_f0453 | d2_f0955 | d1_f0162 | d2_b0300 |
|---|---|---|---|---|---|---|
| start of day (master) | 26.4 | 25.3 | 28.5 | 32.8 | 19.3 | 28.3 |
| background runs merged | 26.5 | 26.1 | 28.8 | 30.5 | 19.6 | |
| composite under animated tiles, additive colour choice | 26.6 | 23.1 | 28.8 | 30.7 | 19.5 | |
| sprite16 from the packet's words | 26.1 | 22.7 | 28.3 | 30.9 | 19.0 | |
| emit_scene's visit order | 25.6 | 22.1 | 27.8 | 30.8 | 18.4 | |
| fm_kind's vertex loops unrolled | 24.8 | 21.8 | 27.4 | 30.2 | | |
| ovl_0a game code -Os (7.3 KB of slot 0) | 24.8 | 21.9 | 27.4 | 30.1 | 18.4 | |
| port_native_field_mesh per kind | 24.3 | 21.8 | 27.1 | 29.7 | 18.3 | |
| native_run's vertex loop unrolled | | | | | | 27.9 |

Tried and not kept: caching the additive copies' colour choice (the CLUT animates: no hits); port_native_mesh per kind
(battle: -0.3 ms for 7 KB less heap: ovl_0b sizes slot 0, so its growth is the heap's) and per vertex count (-0.12 ms
for 1.4 KB). Slot 0's room after ovl_0a in a field build is now about 2.7 KB (gen_link's assert); the decomp's
compile flags aren't dependencies of its objects, so a change to DECOMP_OPT_* needs its objects rebuilt (touch
build/portified/src/ovl_XX/*.c).

The renderer's profile is now flat in still fields (no function above 7% of busy time but the models' two passes,
9-17%); d4_f2110 is mostly its models (2,714 polygons a scene). Battles: native_run and port_native_mesh ~9% each,
emit_scene 7.6%, the software rasteriser's rows ~8% (the 2D layer and additive copies).

Later the same day:
- hybrid_sprite16 clips tiles crossing or outside the drawing area itself: d2_f0931 53.4 -> 50.5, d2_f1057 23.3 ->
  22.4 (layers larger than the screen send many tiles outside it).
- The additive light's per-primitive divisions by constants as multiplies (gx_hybrid.c is Thumb, which has no long
  multiply: a constant division is a call): d2_f0931 -> 49.7.
- The models' chunk-room check against a local end: within noise.
- Tried without gain: the 2D layer's block hashes kept per buffer (fields alternate buffers, so every present
  re-hashed its touched blocks; but the game redraws them every frame, so they are dirty anyway); the hash loop four
  words a step.
- The user chose approximate model shading (2026-10-28, over frame skipping and an early bottom-screen UI): in DS mode
  the models' Gouraud polygons are drawn flat in their first vertex's colour (MODELGOURAUD=1 keeps them exact).
  Battles d2_b0300 27.8 -> 27.5, d1_b0032 22.6 -> 22.3, fields about 0.1; the measured scenes' frames are unchanged
  (their models' vertex colours are equal). Letting the DS cull instead of the PS1's NCLIP would emit every polygon
  (the emit costs about six times the culling test), and one depth per group breaks the models' own occlusion.

### The original PS1's own speed in the benchmark fixtures (2026-10-28)

The user asked for PS1 baselines (the game lags at times on a PS1). tools/psx_field_fixture.py and
tools/psx_battle_perf.py put the original game (the decomp's PCSX-Redux, `tools/emu.py`) in the DS fixture's exact
state: the host exports its bootstrapped party and story (`FF9_SCENARIO_EXPORT=DIR build/pc/ff9ds --scenario ...`
writes characters.bin and events.bin), a field state on the same disc is loaded, the field is left through its own
cleanup to main's dispatcher, the blobs and the target's CD extents (FF9.IMG directories 4 and 2, as
opening_findSceneSectors finds them; battles through tools/psx_battle_fixture.py) are written, and the game's loader
runs. Then, with no breakpoint set, a per-VBlank Lua listener counts VBlanks and GameData.unk8's flips (one per
drawn frame) over 600 VBlanks after 900 to settle. Disc 4 had no field state: `ds_perf_disc4_treno` (card 1 FILE05,
loaded through Continue) is new. The screenshots match the DS fixtures' scenes.

VBlanks per game frame (fields pace at 2, battles at 4):

| fixture | PS1 | PS1 ms per nominal VBlank | DS now |
|---|---|---|---|
| d1_f0110 | 1.99 | 16.6 | 16.7 |
| d1_f0162 | 2.00 | 16.7 | 18.1 |
| d1_f0453 | 1.99 | 16.6 | 27.0 |
| d2_f0931 | 1.99 | 16.6 | 49.7 |
| d2_f0955 | 2.00 | 16.7 | 29.1 |
| d2_f1057 | 2.00 | 16.7 | 22.4 |
| d4_f0351 | 2.00 | 16.7 | 21.2 |
| d4_f1911 | 2.00 | 16.7 | 16.7 |
| d4_f2110 | 2.00 | 16.7 | 24.2 |
| d1_b0032 | 3.97 | 16.6 | 22.3 |
| d1_b0071 | 4.00 | 16.7 | |
| d2_b0128 | 3.97 | 16.6 | |
| d2_b0300 | 3.97 | 16.6 | 27.5 |

Every fixture runs at the PS1's full pace, so 16.7 ms stays the target for all of them. Caveat: Redux's interpreter
charges a fixed cost per instruction and doesn't model the GPU's drawing time, so a PS1 slowdown that comes from the
GPU (large translucent areas) or from cache misses wouldn't show; DuckStation models both more closely and would be
the cross-check for a scene where the PS1 is suspected to lag. d2_f0931's fixture is a black screen with one line
of text on both machines: the PS1 draws it at full pace while the DS spends 49.7 ms, so its hidden scene is the
DS's waste to find.

### d2_f0931: why 50 ms (2026-10-28)

The fixture's screen is black with one line of text, on the PS1 as on the DS, but the scene under it is a large dark
field whose camera scrolls: every scene its background sprites' positions and texture words change (colours don't),
675 of them after merging, 1,073 primitives recorded, 1,608 polygons in 3D (no UI window covers it: its UI is 26
glyphs). PROFILE=1: the submit (planning, emission) 26 ms per VBlank and the walk 12 ms, against 13 and 2 in
d2_f0955. The scene is under the 3D engine's polygons but over its vertices (about 6,430 of 6,144), so field_frame
keeps the composite (bgc_keep 60); with the camera moving, 18 of the 32 bands change each scene and the composite
is recomposed in software every time: about 200 sprites drawn by the software rasteriser per frame (row_direct8,
rect_rows, poly_row_*: about 20% of the samples). Letting such a scene (over budget, background moving) go two-pass
instead hung it at frame 445 every time. Found (the stall's savestate: the ARM9 in flush_chunk waiting on DMA0,
whose control word was DMA_FIFO | 0x3E5085): the split's counting pass (tp_note_all) ran fm_emit with its write
pointer on a one-word stack scratch, and fm_emit still wrote each model group's matrix load through it; its room
check saw a "chunk" of the stack's distance from the chunks and flushed it: a DMA of 4 million words whose count ran
into the destination-control bits, walking the geometry engine's command ports. Any two-pass scene with native
models could hit it, depending on layout. Now counting writes nothing (d1_f0574 still in two passes, 75.2 ms per
VBlank, its frame as before).

With the hang gone, d2_f0931 still doesn't draw in two passes: its textures don't fit banks C and D (all 6 of its
two-pass scenes left for software, 60 frames held each). What made it cheaper with the change (46.5 ms per VBlank
against 49.7) is the 3D engine alone: without the composite, its background runs stay on (the composite latches
them off for the field), and with them its scene fits the 3D engine's RAM (217 of 223 scenes, the whole measured
window, plain 3D). The change (the composite only for a still background past the limits) is not kept: other moving
scenes past the limits lose their composite with it (Alexandria above). The composite's latch on the background
runs is the lead for d2_f0931.

Measurement pitfall: a GP0 command histogram counted over packets' words counts data words (image uploads') as
commands; counted at packets' starts, d2_f0931 has no POLY_GT4 packets (its models are all native).

### Models that fell back to packets (2026-10-28, evening)

Counting port_native_field_mesh's refusals over the 29-field set (a debug build): d1_f0574's close-up models passed
FM_POLYS (4096 polygons a scene, culled ones included), so whole groups went through their packets (GP0 decoding,
recording, planning): FM_POLYS 6144 (768 bytes of the stores' vertex room) took it from 68.8 to 50.1 ms (8 pixels
differ: the groups now native). Its two-pass histogram also divided in 64 bits per polygon (Thumb: a library call):
75.2 -> 68.8 before that. d3_f1812's characters are drawn by fldmdl_drawMeshClipY (cut at a height: Alexandria's
courtyard), which had no native path: now the same wrapper as fldmdl_drawMesh with the clip rules (a polygon with a
vertex fldmdl_projectMeshClipY flagged in its depth word's high half is left out, the OT without the group's bias,
triangles' entries from the depths' sum / 3): 55.6 -> 45.5 ms, frames identical. No fixture of the set uses
fldmdl_drawMeshMirrored (counted on the host); its native path is the same pattern with the culling's sign reversed.
What's left in d3_f1812: its fountain's groups have no opaque polygon (they keep their packets, the PS1's order for
translucent polygons) and the exact additive layer's software drawing (triangle, window_rows, tile_semi: ~8%).

29-field set now (ms per VBlank, PS1 16.7): at or within 1 ms: d1_f0110, d1_f0158, d1_f0306, d1_f0615, d2_f1203,
d3_f1551, d3_f1606, d3_f2055, d3_f2714, d4_f1911, d4_f1953, d4_f2934 (16.7-16.9), d1_f0205 17.3, d1_f0162 17.9;
d2_f1425 19.0, d4_f2162 19.0, d4_f0351 21.2, d2_f1057 22.3, d2_f1102 23.9, d4_f2110 24.1, d3_f1811 24.4, d3_f1508
25.5, d2_f1414 26.4, d1_f0453 26.9, d2_f0955 29.1, d4_f1706 31.0, d3_f1812 45.5, d2_f0931 46.2, d1_f0574 50.1.

### Walking, a wider sample, and more slow paths (2026-10-28, night)

The benchmark fixtures stand still (no input). With the player walking (scripted d-pad holds from frame 500,
build/validation/dsmode_walk_w.sh, PROFWIN=128): d4_f2110 31.1 (24.1 standing), d4_f0351 25.0 (20.1), d1_f0453
27.1, d1_f0162 19.6, d2_f1057 22.0, d2_f1102 21.4, d3_f1606 17.4, d1_f0110 17.0. Walking scrolls the camera, the
composite can't hold, and d4_f2110 goes past the 3D engine's budget: the composite is then kept and recomposed in
software every scene. Taking it away for a moving background (fbc3c31) sent walking d4_f2110 two-pass, its textures
didn't fit banks C and D and it drew in software at 83 ms: reverted (1243743). Renderer changes are now checked
walking as well as standing.

A wider sample (every 19th of the 1,131 field fixtures, 60, standing): median 26.5 ms per VBlank, 9 at 16.7-17, 16 at
or under 19.2; the worst d3_f1818 46.2, d1_f0604 43.6, d3_f0767 42.9, d1_f0501 41.5, d2_f0759 41.3, d4_f2904 40.9,
d4_f0759 38.1, d1_f0056 37.3. All draw in 3D. Two slow paths found there and fixed exactly:
- Lines wholly outside the drawing area: Burmecia's rain (d2_f0759) sent ~50,000 a run to the software rasteriser,
  which stepped each pixel testing the area. Now nothing is drawn at once: d2_f0759 41.3 -> 23.6, d3_f0767 42.9 ->
  38.6.
- fldmdl_drawMeshMirrored had no native path (3 fixtures in the sweep): d3_f2602 17.3 -> 16.7.
The host sweep (1,131 field fixtures, 1,500 frames each, with counters) found 43 fixtures drawing clip-Y groups (now
native) and 3 drawing reflections (now native). The main executable's GTE calls (math_buildRotMatrixZYX's GPF) don't
take gte.h's fast paths (DECOMP_GTE_BINS has the overlays only); adding main changed nothing measurable (~150 calls a
frame), not kept.

### Semi-transparent background sprites in the composite; the reuse cache's ceiling (2026-10-28, night)

The user's choices of 2026-10-28: native translucent model polygons (approximate order) and research into reusing
unchanged content between frames.

What the effect-heavy fields' 450-540 semi-transparent primitives a scene are (counted by kind): in d4_f2904 and
d1_f0604 none are model polygons: they are background sprites (d4_f2904's mist: 165 additive, 279 subtractive, about
112,000 pixels; d1_f0604: 280 additive sprites and 260 effect polygons); d3_f1818's 1,414 translucent model polygons
are all additive (mode 3) in 2 groups. So native translucent models would help few fields, and their blending would
need the additive copies' machinery; not started.

Semi-transparent background sprites now join the composite when they have held still 4 scenes (their own hash) and
every one lies behind the scene's 3D primitives in its cells; their blending is then drawn exactly in software, and
the opaque sprites behind them leave the 3D scene too. The composite's stillness counts the opaque sprites alone.
d1_f0453 27.2 -> 23.4 standing, 27.1 -> 23.3 walking (its trees' shadows); d2_f1102 23.9 -> 21.5. Without the
stillness condition, moving mist was composed in software every scene: d4_f2904 41 -> 86 ms (not kept). The room came
from port_native_field_mesh's kinds in two copies instead of eight (the same speed, 3.9 KB).

The reuse cache's ceiling, from the profiles: what a still scene redoes for unchanged content (recording the
background's sprites, the composite's checks, planning, emitting the sprites left in 3D) is about 15-20% of its busy
time (d4_f2110: ~3-5 ms of 24); the models (~32% there) animate every frame, and a walking scene's background changes
every frame. Its memory: an emitted command list for the sprites left in 3D is ~14 words each (300 sprites: ~17 KB a
buffer), against 13-16 KB of free heap; slot 0 and the overlay data have ~4 KB. Not started: worth at most a few ms
in still scenes and none walking.

GTE NCLIP and AVSZ in 32 bits (gte.h, 2026-10-28): the inline forms computed in `long long`, so every Thumb caller
(the decomp's mesh code, link_translucent, the clipping) called __aeabi_lmul six times an NCLIP; a fine sample
(PCLO) gave __muldi3 2.5% of d3_f1818. When the coordinates lie within +-8192 (NCLIP as (x1-x0)(y2-y0) -
(x2-x0)(y1-y0)) and AVSZ's depth sum is under 65536 the value fits 32 bits with no MAC0 flag, bit-exact against
gte.c over 9M random cases: d3_f1818 48.2 -> 45.4, battles -0.2-0.3.
Tried, not taken: the renderer's hot main-RAM functions as ARM (emit_scene, plan_runs, plan_prim, translucent_id,
regions_touch, conv_choose, the recording): 0-0.2 ms (noise) for 6.8 KB. Its hottest overlay functions are ARM
already; melonDS charges main-RAM ARM fetches as Thumb's, so ARM pays only where Thumb needs library calls.
The sampler's main table has 2 KB buckets, split over the functions they overlap by bytes: small functions near hot
code (dlmalloc's, the double-float helpers) show samples they never take; PCLO/PCSHIFT=4 resolves a range.

ITCM for the renderer (2026-10-28). In this melonDS main-RAM code costs 1.5-3 cycles an instruction (Thumb or ARM
alike) and ITCM code 1, so what runs per primitive belongs in ITCM more than the software polygon rasteriser, which
DS mode only uses for its additive layer and scenes gone software. DS-mode hybrid builds now keep triangle and the
poly_row_* functions (11.8 KB) in main RAM (softgpu.c SG_POLY_HOT) and put in their place, as ARM (gx_hybrid.c
GXH_HOT): translucent_id, plan_prim, light_under, hue_of, target, record; and the GP0 decoder (gp0.c GP0_HOT:
command, gp0_tpage_sprite16, gp0_note_sprites). ITCM: 31.5 of 32 KB. Every scene measured gained, the software-heavy
ones too: d1_f0604 41.0 -> 38.0, d3_f1818 43.1 -> 39.9, d4_f2110 24.8 -> 23.8, d2_f0931 48.2 -> 44.8, d4_b0331 34.3 ->
32.4; walking d1_f0604 37.0 -> 34.9. Earlier, translucent_id (a translucent primitive's polygon ID, 14% of d1_f0604)
lost its 16-slot scan: the recent polygons indexed by vertex hash, the same IDs (d1_f0604 45.4 -> 41.2).
What stays in ITCM and is worth weighing next: emit_scene (8.5 KB Thumb, ~10% of a field) doesn't fit;
ds_scale_display_text_pair (2 KB, the top-screen text) is 0.4-2% (battles), interpolate and light (2.9 KB, the GTE's
lighting) under 0.4%.
Tried, not taken: port_native_field_mesh (9.8% of d3_f1818, 4 KB of ARM) in ITCM, with gte_cmd out (d4_f2110 +0.5)
or with ds_scale_display_text_pair, mat_mul and hue_of out instead: -0.3 on d3_f1818, +0.35 on d4_f2110. Its time
is the packets' and vertices' reads from main RAM, which ITCM doesn't change; translucent model groups (no opaque
polygon) now go through link_translucent in ITCM instead (d3_f1818 -0.5, d4_f2904 -0.6).
Profiling note: DS-mode window builds (PCLO) have 1280 buckets, not 2560: PCSHIFT=8 from 0x02004000 covers the main
text (320 KB), PCSHIFT=8 from 0x02258000 the overlay slot; samples past the window count as "outside".

Native translucent model polygons (2026-10-28, the user's approved approximate blend order). A model group's
additive translucent polygons (modes 1 and 3, textured: glows) are taken by port_native_field_mesh (fm_take_semi)
while the scene's additive area takes the 3D approximation anyway (over a sixth of the screen, counted with theirs:
fm_semi_ok), and drawn by fm_semi_emit as emit_scene draws a recorded additive polygon (brightness as alpha,
light_under's whitening, the converted texture, from the group's vertex snapshot), with one polygon ID per group
instead of translucent_id's shared-edge IDs: where a group's own layers overlap only one blends. Smaller additive
areas keep the exact software layer from the linked packets; a scene gone software draws them from the packets
(fm_soft_draw). d3_f1818 39.1 -> 30.1 standing, 37.2 -> 31.3 walking; 2,360 pixels of its water glow differ slightly.
Linking its translucent model polygons cost ~10 ms a VBlank there (they went through GP0, recording, planning,
translucent_id and light_under one by one).
d4_f2404 (30 ms) loses 10 ms without its translucent model polygons too, but they are mode 0 (50%): with them the
scene passes the 3D engine's 6144 vertices, two passes don't fit its textures, and it is drawn in software. Taking
them natively wouldn't change the count: it needs the two-pass texture fit (banks C-D).

d4_f2904 (36 ms): its composite holds every scene but takes 82 of its 454 background sprites; the rest lie in front
of its far moving mist (semi-transparent sprites) and must be drawn over it, so they stay in 3D. Tried, not taken:
letting semi-transparent sprites not keep the background out of the composite (30.8 ms, but the mist then covered
the foreground bridge: blurred difference 12.9). Taking those sprites out of the 3D scene would need a 2D layer in
front of the 3D one (no VRAM bank left for it).
Also measured, not taken: LoadImage skipping unchanged rows (d3_f2402's animated pages do change every frame) and
dirty-row uploads of regions (its writes cover whole regions).

Later on 2026-10-28, each exact (identical frames) unless said:
- clip_line: a line inside the drawing area unchanged, the rest clipped in 32 bits (Burmecia's rain: ten 64-bit
  divisions a line before): d3_f0767 35.9 -> 33.5.
- Software raster: a tinted semi-transparent sprite through the shaded palette (row_semi_palette), and it and
  poly_row_semi4 one loop per blend mode: d3_f2402 34.8 -> 33.8, d2_f0356 31.5 -> 31.3. test-raster links again.
- upload_run in whole words (an animated background page's runs are copied every frame it changes): d3_f2402 -> 33.4.
- translucent_id's tables in DTCM (584 bytes of the ~9 KB of stack no run has reached): d1_f0604 37.2 -> 36.8.
The 60-field sample: median 26.5 -> 23.2 ms per VBlank over the day; 13 fields at full speed, 20 within 15%.
Where the worst now go: d1_f0501 (39: a dialog's UI work, ~12%), d1_f0604 and d4_f2904 (translucent sprites: their
polygon IDs, light and emission), d3_f2402 and d2_f0356 (the exact additive layer in software: ~15-30%).

### Battle two passes: the texture fit (2026-10-29)
A per-scene log over 32 over-budget battles (each mesh's culled vertices, the recorded primitives', a histogram of the
overage) showed that two passes rarely run: in d1_b0228 and d3_b0232, 216 of 221 and 230 of 232 over-budget scenes
found no split whose two texture sets each fit banks C and D (large first passes: the first pass's don't fit; small
ones: the second's), so they dropped their smallest polygons as before, after up to eight texture plans a scene
(the 15-20 ms the two passes seemed to cost). The earlier "no drops where two passes ran" was the captured frame only.
battle_two now skips the tries for 0, 1, 3 ... 31 scenes after each failure in a row (reset when a split fits) and tries
the last split that fitted first: d1_b0228 35.7 -> 25.1, d3_b0232 36.2 -> 25.5, d4_b0678 41.8 -> 36.6, d2_b0135 47.4
-> 32.4 ms (its splits mostly fit). Frames identical where the state is (d2_b0135's capture differs in a toad's pose).
Where the vertices go (average scene): the party ~2,300-2,600, the heavy enemy ~2,000-3,000, the recorded primitives
(the background's layers 8/4/2, linked into the second OT, which port_native_bg leaves to GP0; effects; UI)
~900-1,700; the over-budget scenes are 100-1,000 vertices over on average.

### Battle model triangle strips (2026-10-29)
The 3D engine's 6144 vertices are what battles run out of, and the models' triangles (FT3, nearly all of an enemy's)
cost three each. In one pass, native_strips draws each model's opaque textured triangles before the walk, in its FT3
list's order, as triangle strips: a triangle sharing an edge with the last one (same texture use and colour, the
edge's uvs the same; the strip's first triangle turned so that the edge the next one shares is last) costs one
vertex. The vertices a triangle brings to its strip sit at its own drawing place: its OT entry mapped to the walk's place
there (strip_places, per 4 entries, in ovl_0b's dead code after the entry heads), so the models' order among the
arena's and effects' primitives stays about the PS1's (approximate depth, approved for models). (A depth per vertex
took 2 bytes a vertex of the mesh pool, which then had no room for the walk's lists: d3_b0037 dropped 2,216.) The walk skips those triangles but counts their places. The budget counts strips
(strip_count: the same rules); two passes don't use them (their split is by place) and count every vertex.
As stored, the lists' order strips 15-21% of the heavy enemies' drawn vertices (offline, eight views each; reordering
the lists into strips would make it 27-30%). Dropped polygons over a run: d1_b0228 7,943 -> 0, d3_b0232 19,435 -> 0,
d4_b0678 3,408 -> 1,566, d2_b0135 17,302 -> 6,826; time 25.1 -> 28.8, 25.5 -> 29.2, 36.6 -> 33.7, 32.4 -> 36.8 ms
(the scenes are now drawn whole).
Then (same day) the lists in strip order: a battle unit's model (party, enemy, weapon: not an effect's, whose split
vertices modules index in list order, nor the Zombie's, model 14, whose polygons effect 0x17F numbers) has its FT3
lists reordered once, at its first btl_projectMesh (mesh_native.c strip_order: greedy strips, each next triangle
searched 16 records either side; scratch in the projection's screen arrays). The PS1 output changes only in which of
two polygons at one OT entry is drawn first. d4_b0678's drops 1,566 -> 804; times 28.4, 28.8, 33.2, 36.3 ms. The
1.4 KB of ovl_0b code costs heap byte for byte (ovl_0b sizes slot 0): scenario.c's fixture state shrank by 2 KB to
make room (the encounter metadata's 10 KB temporary buffer at bootstrap had failed).
Fixture loading: scenario.c reads the encounter bundle's dependency table a sector at a time into one sector of stack
(its 10 KB temporary buffer no longer fit in the heap at bootstrap: 5 of 32 battle fixtures stopped). 32 over-budget
battles all run; 21 drop nothing or under 1,000 polygons a run; the rest are heavy enemies (Ochu, Bandersnatch,
Grand Dragon, Gigan Toad, Red Dragon, Zuu, Axe Beak x2), the simplification candidates.

### Simplified battle enemy models (2026-10-29)
After the strips, only battles with the heaviest enemies still dropped polygons. The user chose to simplify those
enemies outright (protecting small features), never the party. tools/simplify_battle_models.py rewrites a model file
in place (same size and split: per mesh group only the vertex and FT3 arrays shrink; uvs, colours, motions and TIMs
stay) by half-edge collapses, cheapest first by quadric error, where the vertex's triangles are all on its bone, one
texture/colour/flags, seamless at it, a closed fan, in no quad, and bend less than 60 degrees (teeth and folds stay).
The patched sectors go to build/patches/discN.bin ("FF9P", runs of {IMG sector, sectors, offset}); port/psx/cd.c
serves them from ff9ds/patches/ in place of the image's. config/model_patches.txt lists the models and options and
each disc's SHA-256: `make model-patches` (part of `make ds`) regenerates them from the disc images and fails on a
mismatch; the patches themselves are game data and never committed. Dropped polygons over a run: Ochu 23,822 -> 1,680,
Bandersnatch 20,766 -> 817, Gigan Toad 6,826 -> 514, Zuu 3,366 -> 1, Axe Beak x2 4,818 -> 240; Grand Dragon and Red
Dragon barely simplify that way (2-4%: 18,249 -> 13,443, 6,806 -> 6,186). Known: a notch at the Ochu's front petal and
some texture shift where corners take a neighbour's uv. Next (the user's suggestion): bake removed and moved surface
into the textures behind, which also lets features like teeth go.
Then (the user's suggestion) texture baking: a seam's vertex may collapse along the seam (each texture chart around it
takes that chart's uv at the target), and the texels of every triangle a collapse changed are redrawn from the
original surface (from the simplified triangle's point along its normal, the nearest original triangle of the same
bone; that texel's colour, the target CLUT's nearest, or a CLUT entry no pixel uses when none is near). Quads are
simplified as their two triangles and are quads again where both come through unchanged. Patch runs within 8 sectors
of each other are merged (the image's own sectors between) to stay within cd.c's 32. At 15% with sharp features
kept, Ochu's close-ups are near the original (a notch and a faint mark on its front petal). Dropped polygons over a
run: Ochu 638, Bandersnatch 189, Gigan Toad 586, Zuu 0, Axe Beak 236; Grand Dragon 8,215 and 2,619, Red Dragon 6,136:
most of their vertices are in triangles across bones, which needs their pose (R27).
Baking is off in the manifest for now (bake 0): on the Gigan Toad it darkened the back and spread its nostrils' dark
texels into a block (no-bake renders match the original closely), so its colour mapping or source choice is still
wrong somewhere; the code stays behind the option. A collapse may no longer change a triangle's texture stretch by
more than 1.5x, and a group whose split quads don't fit its lists is redone with its quads whole. Dropped polygons
over a run, no baking: Ochu 2,037, Bandersnatch 153, Gigan Toad 529, Zuu 0, Axe Beak 258; Grand Dragon 9,497 and
3,348, Red Dragon 6,231 (R27).
The baking bug: texel centres were sampled at x + 0.5 and the source texel taken as the rounded coordinate (one
texel off everywhere: darker edges, the toad's nostrils spread into a block). A self-test that re-bakes an untouched
model as if every triangle had changed now reproduces 98.5% of its texels' colours (the rest along edges; it used to
change most). Baking is back on: the toad matches the original, the Ochu's petals keep their centre stripe; the
notch in its front petal (geometry) remains. Drops: Ochu 656, Bandersnatch 215, Gigan Toad 605, Zuu 0, Axe Beak 258.
With the decomp's R27 (tools/battle_pose.py, exact against the PS1), the simplifier judges collapses on the posed
model (the idle motion at three frames, model space): a vertex's triangles may span bones (u still only moves onto a
vertex of its own bone; flips are checked in all three frames), and baking samples the posed original surface, only
from the original triangles the simplified one took over (its provenance: a collapse merges its fan's), which keeps a
moved chest from picking up an arm's texels. Dropped polygons over a run: Ochu 153, Bandersnatch 34, Grand Dragon
22 / 127 / 282 / 2,703, Gigan Toad 776, Zuu 0, Axe Beak 0, Red Dragon x2 5,067 (it reaches only 6%: sharp folds).
Per-model options in the manifest ("model 610 sharp 75"): the Red Dragon at 20% with folds up to 75 degrees
collapsible (close-ups checked) drops 3,617 (from 5,067). Still over now and then: Red Dragon x2 (d3_b0194), Grand
Dragon (d4_b0617: 2,703), Gigan Toad (776).
Strip links at load (same day): the strips' structure is fixed once a model's lists are in strip order, so
strip_order also writes each FT3's link into its last byte (padding: bit 0 it continues the one before, bits 1-3 its
corners in strip order), and the per-frame count and the emitter only read it (no edge or uv search per triangle,
which was 14% of d1_b0228's busy time). Only meshes whose header carries the flag (NativeMesh.strips) draw strips.
d1_b0228 28.4 -> 26.0, d3_b0232 28.8 -> 26.4, d4_b0678 33.2 -> 30.8, d2_b0135 36.3 -> 33.7 ms; no more drops.
Battle sweep (60 fixtures, DS mode, 2026-10-29, after the strips, the simplified enemies and the strip links): all 60
run to frame 1500, median 25.2 ms per VBlank (it was ~28-29 before the strips' precomputation), 43 drop nothing over
a run. The rest: Red Dragon x2 (d3_b0194, 3,615), d4_b0359 (3,355: an effect-heavy close-up of the party, no
enemy model on screen), Grand Dragon (d4_b0617, 2,703), Worm Hydra (1,617), Zaghnol (1,308), Gigan Toad (776), and
under 600 elsewhere. On the host, all 1,085 battle fixtures pass with the patches (2,000 frames, rendering).

### Where a field frame's time goes (2026-10-29, PROFILE=1 with temporary timers in submit)
Per VBlank (fields run the game at 30 fps: one submit per two VBlanks): d4_f2904 35.5 ms = submit 22.9 (field_frame
12.9: planning, the composite, the 2D layer; emit_scene 9.6; upload 0.5) + the OT walk recording 6.5 + game ~4.5.
d1_f0604 37.5 = field_frame 7.9 + emit_scene 11.2 + OT walk 7.3 + game. d2_f0356 30.2 = field_frame 15.6 (of it the
exact additive layer's software raster 7.8) + emit 1.5 + OT walk 5.2. d4_f2904 records ~946 primitives a scene:
emission is on the order of 1,000+ cycles each, the field's largest single cost after field_frame.
Next structural lever for fields (measured 2026-10-29, d4_f2904): with the background composite in use (601 of 608
scenes there) every tile is still recorded through GP0 one by one each frame (hybrid_sprite16 6.3%,
gp0_tpage_sprite16 2.3%, target 1.2%, record 1.1%: bg runs are off while the composite holds the scene) and then
hashed and sorted by the composite's passes (bgc_hash/sources/use ~8%), only to find most of it unchanged. Plan: a
per-run summary (its tiles' rectangles, slots and hash) cached by field_bg.c's markers; a still run's tiles taken
from the summary (the composite's per-tile behind/in-front decision made from it, the tiles in front of a model
recorded as now), skipping their packets. Estimated 10-15% in composite fields. The emission side (d4_f2904: ~390
translucent mist sprites and ~300 opaque sprites a scene in front, ~110 model polygons) is already on fast paths.
Per-field background path by measure (same day): whether the composite pays depends on the field (with it, a still
background's sprites are recorded one by one and composed; without it its runs are quads in the 3D scene: d4_f2904
25 ms per VBlank without, 33 with; d2_f1215 26 without, 23 with). After TUNE_SETTLE scenes, a field's scenes
alternate the two (TUNE_SCENES each, twice) counting real VBlanks; the one without stays if it took 10% fewer,
measured again every TUNE_AGAIN scenes. Without the composite only within the 3D engine's budget (past it the
composite keeps the scene in one pass: d4_f2101 went 30 -> 83 otherwise), and back to it once the budget latches the
runs off (d3_f1818). 60-field sample: median 23.1 -> 22.5 ms (d4_f2904 35.4 -> 25.0, d3_f2402 32.3 -> 25.8,
d3_f2173 25.7 -> 23.3), no field worse by more than 0.9 ms (noise). A first attempt at making the composite take
the runs themselves (run summaries) cost 3 ms in other fields (their runs moved, or the paths fought) and was dropped.

### What the field models cost (2026-10-29)

Upper bound: the 15 slowest fields of the 60-field sample with fldmdl_projectMesh and the three fldmdl_drawMesh
variants returning at once (no models at all), ms per VBlank, full -> without models:

| field | full | no models | models |
|---|---|---|---|
| d1_f0501 | 39.2 | 30.3 | 8.8 |
| d1_f0604 | 35.8 | 30.2 | 5.6 |
| d3_f0767 | 33.1 | 23.9 | 9.2 |
| d4_f2404 | 30.3 | 16.7 | 13.6 |
| d3_f1818 | 30.2 | 21.7 | 8.5 |
| d3_f2801 | 30.1 | 21.7 | 8.4 |
| d4_f2101 | 30.0 | 16.8 | 13.2 |
| d2_f0356 | 29.8 | 26.4 | 3.4 |
| d4_f2155 | 28.9 | 20.6 | 8.2 |
| d1_f0359 | 28.6 | 16.8 | 11.8 |
| d2_f1656 | 28.6 | 18.2 | 10.4 |
| d2_f1462 | 28.5 | 17.2 | 11.3 |
| d1_f0814 | 28.2 | 23.8 | 4.4 |
| d2_f1053 | 27.7 | 20.7 | 6.9 |
| d1_f0056 | 27.4 | 22.7 | 4.8 |

The models are the largest single cost (median 8.5 ms per VBlank, 17 a scene) and the whole gap in five of the
fifteen. The rest of d1_f0501 (its dialog box's software UI replay is 5.4 of it) and d1_f0604 (a still background:
emit_scene's per-primitive translucent paths, bgc_use, the OT walk) is the background's and the UI's. (Measuring:
an ovl_0a-only change still needs `touch` on nothing; but a scratch copy of a source restored over a test edit drops
the test's other lines: restore test edits with git checkout.)

### Field models through the 3D engine, again (branch gx-models-2, 2026-10-29): correct, not faster

Why the first branch drew garbage: its bone matrices were the GTE's x 4096 (so that the result is in the GTE's
units with v16 vertices), which puts clip coordinates near 2^25 (w ~ 28 million for Z 7,000); POS_TEST read them back
exactly (a vertex at 224.4, 256.0 against the GTE's 224, 255), but the 3D engine's later stages overflow on them.
x 16 (w ~ Z x 16, the projection's depth constant / 256 to match) draws the models right, close-ups matching the
projected path.

With the vertex pass deferred (fldmdl_projectMesh keeps the bones; the renderer transforms the vertices to view space
with them, no division: Z for the group's depth range, the view-space determinant for the PS1's culling, matching
NCLIP's sign and its sub-pixel zero; projected on demand for the packets' path, mirrored draws, translucent groups,
software and two-pass scenes) it is still slower: d1_f0359 31.3 against 28.6-29.4 ms per VBlank, d4_f2404 32.3
against 30.3. The CPU's share (fm_take_hw, ~170 cycles a vertex with its polygons' culling) costs about what the ITCM
GTE's projection and the take pass did, and the per-vertex matrix restores add 3D engine work. The DS's own culling
would drop the per-polygon test but emit the back faces too (twice the polygons through fm_emit and the 3D engine's
2,048). Not merged; the branch also has fm_kind in four instantiations instead of eight (4.2 KB of slot 0 back).

### Small translucent particles approximated (2026-10-29, the user's choice)

d1_f0604's flower sparkles (260 textured translucent quads a scene, 9-24 pixels) went through translucent_id's
shared-edge search and light_under's stacked-light estimate one by one. A translucent polygon at most SMALL_PARTICLE
(24) pixels wide and tall now takes polygon ID 1 + (n & 31) and the base whitening. d1_f0604 35.9 -> 34.2 ms per
VBlank, d2_f0931 43.6 -> 42.1; d4_f0351, d4_f2110, d1_f0453, d2_f0955 within noise; frames' mean absolute difference
at most 0.03 levels (the sparkles look the same in close-ups).

### Simplifying non-party field models (2026-10-29): little to take

With R28 (decomp docs/field-models.md, tools/field_models.py: field models are battle-format files, FT4/FT3 only,
inside each field file that shows them or in dir 10 for the party), tools/simplify_battle_models.py's collapses were
tried on the heaviest non-party field models at a 30% target, posed on their default motion: model 200 (2,732
vertices, 15 fields) 1-2% per group, 534 7-12%, 625 1-7%, 252 2-4%, 294 0-7%, 488 0-4%. Field models are low-poly
and skinned: most vertices sit on bone, UV-seam or quad boundaries the collapses must keep (positions are bone-local,
seams carry the texture). Not pursued (about 0.5 ms in the slowest fields); the tool's field support isn't kept.
Notes for a later try: the header's u16 packet total wraps past 0xFFFF (200: groups sum to 0x12258), and a model's
container item has a different padding in different field files (copy the changed span, not the item).

### Battles still dropping polygons (60-battle sample, 2026-10-29 evening)

59/60 ran (d2_b0920's stop was the melonDS FolderSync truncation of ovls.bin, fixed in tools/ds_profile.py; it runs).
Median 25.2 ms per VBlank. 15 battles dropped some polygons over the run: Red Dragon d3_b0194 3,615, d4_b0359 3,355
(effects), Grand Dragon d4_b0617 2,703, Worm Hydra d2_b0780 1,617, Zaghnol d1_b0014 1,308, Gigan Toad d2_b0135 776,
Armstrong d3_b0583 537, and 8 more under 300. Why two passes fail there (counters in battle_two, three of them): the
scenes are only 5-10% over (2,115-2,204 polygons, 7,100-7,300 vertices without strips), and every split tried fails
on the second (nearer) pass's textures, which must fit banks C and D alone (A holds the first pass's capture, B the
frame being captured; neither can hold textures read while it's written). Raising those enemies' simplification
from 15% to 30% removes almost nothing more (bone, seam and sharp-feature vertices bind: Grand Dragon 11/4%, Armstrong
4/5%, Zaghnol 14/2%); Dracozombie at 15% (11/15%) didn't measurably change d4_b0673's drops: not kept. Open: a second
pass with fewer texture pages (split by texture use, not only by depth), or strips in two passes.
Measured after: a failing second pass wants ~300 KB of texture regions (d4_b0617 14 regions, 298,592 bytes, peak
358 KB; d1_b0014 300,736) against banks C and D's 256 KB, and the regions are tight (the uses' own boxes sum to more:
they overlap). The room has to come from the passes' banks (one capture bank for both passes, the other for textures)
or from a third pass (a battle frame is four VBlanks).
The split search, then: battle_two capped the first pass at half the scene (the second pass then always held the
nearer half, party and enemy, ~300 KB of textures). It now searches the whole range, from as many places as a pass
holds down to the least the second leaves it, by halving for the most first-pass places whose textures fit (they grow
with it, the second's shrink), and checks the second pass there; the last fitting split first. Of the 15 dropping
battles: none drop now in d4_b0617, d2_b0780, d1_b0678, d1_b0304, d2_b0277, d3_b0233, d2_b0321, d4_b0673 (Grand
Dragon 2,703 -> 0, Worm Hydra 1,617 -> 0); unchanged d1_b0014 (Zaghnol), d3_b0194 (Red Dragon: no split fits, the
pages both passes use), d3_b0583, d2_b0280, d4_b0657, d4_b0922, d4_b0359 (effects); d2_b0135 776 -> 702. Two passes
cost: d2_b0135 33.0 -> 37.6 ms, d4_b0617 27.4 -> 29.3; the others within noise.

### Battles in three passes (2026-10-30)

When no two-pass split fits banks C and D, battle_two tries three: the first pass the most places whose textures fit
(as for two), the second the most after it whose textures fit (halving), the third the rest if it fits the 3D engine
and the banks. The passes' captures alternate banks A and B (tp_cap): the first pass's 3D into the bank not showing
the last whole frame (tp_fbank), each later pass over the last capture as BG3, its composite into the other; a third
pass leaves the whole frame in A, and the next scene's first pass shows it with MODE_FB0. Two-pass scenes (and every
field's) keep the frame in B as before (fields' frames identical). The third pass's textures are planned and
uploaded once the second's capture ends, and listed before the game goes on (gxh_tp_finish), as the second's.
Result: no drops in Zaghnol d1_b0014 (1,308 before), Gigan Toad d2_b0135 (702), Armstrong d3_b0583 (537); the cost is
the passes' texture uploads (each pass's pages displace the others' in C and D, re-uploaded every scene): d1_b0014
32 -> 45 ms per VBlank, d2_b0135 37.6 -> 57.9, d3_b0583 27.7 -> 31.7 (the user's choice of 2026-10-29: no drops
over speed). Red Dragon d3_b0194 still drops (no first pass fits: its arena and dragons share pages).
