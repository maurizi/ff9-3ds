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
* Totals now: about 0.85 MB code and read-only data, 2.35 MB bss (2 MB `psx_ram`), 1.5 MB VRAM/SPU shadows,
  and (HYBRID=1) about 1.1 MB of renderer buffers: still about 5.8 MB against 4 MB.

## What already streams and what must change

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
