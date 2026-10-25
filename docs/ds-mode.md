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
