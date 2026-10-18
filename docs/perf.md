# DS performance

Where the DS spends each frame, how to measure it, and what has been done.
Everything here is the software renderer (`SOFTDISPLAY=1`, the default) in
melonDS 1.1 DSi mode (134 MHz ARM9, JIT). melonDS's JIT timing mostly tracks
instruction counts and code placement. In that configuration it charges main
RAM and DTCM data accesses alike (`build/bench/cache_probe.c`; the interpreter
charges main RAM about 1.25x), so treat these figures as guidance, not hardware
measurements.

## Measuring

```sh
tools/docker.sh make -j16 ds AUTORUN=1 PROFILE=1 CAPTURE_FRAME=700 \
    SCENARIO=sd:/ff9ds/scenarios/field259_anim_length.cfg
cp build/ff9ds.nds build/validation/x.nds; cp build/ds/ff9ds.elf build/validation/x.elf
python3 tools/ds_profile.py --rom build/validation/x.nds --elf build/validation/x.elf \
    --frame 700 --out build/validation/x_700.ml8
```

`CAPTURE_FRAME` freezes the game at that frame, so the published 128-frame
window is the same on every run: here it ends at frame 609. `ds_profile.py`
runs melonDS on a private display, waits on the live frame counter, saves the
state, decodes the profile, closes the emulator and restores its config.
Categories overlap (GPU commands include polygons and rectangles; ordering
table includes the GPU work it triggers; SPU voices include steady runs and
event samples), so don't add them up.

Component benchmarks that run in seconds, with exactness hashes:

* `make spu-snapshot-ds SPU_SNAPSHOT=...`: the game's SPU at a captured moment
  (`build/pc/ff9ds --spu-snapshot FRAME:PATH`), including the next 120 frames
  of register writes and uploads.
* `make spu-throughput-ds`: synthetic 4/24-voice SPU load.
* `make mdec-bench-ds`: movie macroblock reconstruction on real movie data.

Where the time goes inside the uninstrumented remainder:
`tools/ds_sample.py --rom R --elf E --from F1 --to F2 [--detail FUNC]` samples the
ARM9 PC through melonDS's GDB stub (interpreter mode). It reports a histogram by
function, callers, and with `--detail` the sampled instructions.

## Field 259 (Evil Forest), window ending at frame 609, ms per 60 Hz frame

| | first measured (this session) | now |
|---|---:|---:|
| interval | 155.3 | 64.2 |
| GPU commands | 106.8 | 17.9 |
| of which rectangles (background tiles) | 87.4 | 7.8 |
| of which polygons | 16.4 | 6.4 |
| GTE | 5.4 | 5.2 |
| SPU (incl. sink) | 17.9 | 18.5 |
| display scaling | 2.3 | 2.4 |

Since frame pacing (below), the same window measures 49.9: the second VSync of each 30 fps pair no longer
rescales an unchanged display or waits a whole VBlank behind time already lost.

The first column is from a different window of the same scene (not yet
fixed-frame), so compare it loosely. Presentation waits for VBlank, so a frame
costs a whole number of VBlanks. About 4 today; the PSX field runs at 30 fps (2).

What changed: background tiles (16x16, 8-bit, unmodulated) took the per-pixel
generic path. They now use a direct-CLUT sprite path and a row loop that reads
each source halfword once. The SPU mixes voices in exact blocks
(`audio-validation.md`). The GTE is compiled as ARM, since Thumb has no long
multiply.

## Frame pacing

Virtual frames (`psx_frame`) wait only while ahead of real VBlanks (`display.c` `pace`), as the PS1's `VSync(n)`
counts VBlanks that passed during the game's work; more than 8 behind drops the debt. The bitmap swap is queued
for the VBlank interrupt. The presenter rescales only when a VRAM write may touch the shown rectangle
(`psx_display_generation`; draws are bounded by the drawing area). Battle still rescales every VBlank: the UI
draws 109 commands into the shown buffer each VBlank (the scene itself every fourth, `VSync(0)` + `VSync(3)`,
15 fps), so the PS1 budget is 66.7 ms per scene, about 4x below the DS's current cost there.

## Ark battle (disc 3), window ending at frame 1633, ms per 60 Hz frame

Needs an SD image with disc 3:
`ds_profile.py ... --sd-folder build/sdroot_disc3 --sd-image build/melon_sd_disc3.img`,
where the folder holds `ff9ds/raw/FF9 CD3.img`, `psx_low_ram.bin` and the scenario.

| | before | dithered polygon rows |
|---|---:|---:|
| interval | 150.6 | 99.9 |
| GPU commands | 105.8 | 54.5 |
| of which polygons | 91.1 | 39.8 |
| of which rectangles | 9.5 | 9.5 |
| GTE | 8.9 | 8.9 |
| display scaling | 10.0 | 10.0 |
| SPU | 9.9 | 9.9 |

Battle polygons are almost all flat, textured, opaque and dithered (82.8M of
84M host pixels in the first 1,700 frames). `tools/check_raster_frames.sh`
checks every fast path against the reference rasterizer on field and battle
frames.

## Intro FMV (title), ms per 60 Hz frame

175 at first (movie decode uninstrumented), 47 after the MDEC, display-skip and
SPU work. See `movie-validation.md` and `audio-validation.md`.

## Sampled field profile (frames 450-660, current build, 800 samples)

16% waiting for VBlank; 14% SPU voices (`fast_span`); 9% background sprite row
loop; 7% reverb; 5% sprite setup (`sg_rect`); 5% `plot` + 3% `triangle`
(polygons); about 13% GTE (`rtp`, `mat_mul`, `gte_cmd`, register moves); 3% display
scaling; ~5% profiling hooks (PROFILE builds only). The game's own code
(`o0a_*`, `func_*`) is only a few percent. `vram_fill` was 13% before its row
fill.

melonDS's JIT charges about two ARM9 cycles per instruction in these loops
(the 8bpp sprite loop: 16 instructions per pixel pair, about 18 cycles a pixel).
So instruction count is the main lever. Reaching the PSX field rate needs about
2x across GPU, GTE and SPU, or work moved to the DS hardware (design M6: native
field backgrounds and models).

## Current numbers (2026-10-17), ms per virtual frame

| scene (window) | interval | GPU commands | polygons | SPU total | display scaling |
|---|---:|---:|---:|---:|---:|
| field 259 (ends 609) | 47.5 | 18.2 | 5.9 | 16.6 | 1.3 (0.5 calls) |
| field 259 transition (ends 865) | 65.2 | 29.7 | 16.4 | 20.7 | |
| Ark battle (ends 1633) | 82.2 | 44.8 | 29.9 | | 7.8 |
| intro FMV (ends 478) | 52.2 | 1.2 | | 16.7 | 2.0 (0.25 calls) |

The field window plays the field's music (18 voices); until the BIOS `memcpy` fix (porting-notes) the host
played none there, so host-side audio checks of field scenes before 2026-10-17 had no music.

Code placement matters as much as instruction count in melonDS: the untextured and semi-transparent polygon rows
first ran from main RAM and were slower than the inlined generic `plot` in ITCM (34.1 vs 27.2 ms); in ITCM
they took 16.4. ITCM is full (about 0x7a38 of 0x8000), so a new hot routine needs something colder moved out.

## Hybrid battles (HYBRID=1 HWSOUND=1, window ending at frame 1761), ms per 60 Hz frame

| battle | 2026-10-17 sweep 1 | after the changes below | 2026-10-18, 78 native modules | 279 native modules |
|---|---:|---:|---:|---:|
| Ark | 78.4 | 59.1 | 51.3 | 50.6 |
| Garland | 42.2 | 42.3 | 42.3 | 42.3 |
| Ifrit (first) | 112.6 | 68.6 | 50.1 | 48.3 |
| Kuja | 39.2 | 37.2 | 33.7 | 33.3 |
| Meltigemini | 50.2 | 49.1 | 40.8 | 40.3 |
| Spare Change | 44.9 | 43.6 | | |
| Sword Magic | 45.5 | 44.2 | | |
| Taharka | 71.1 | 59.4 | 49.5 | 48.3 |
| Valia Pira | 25.2 | 25.8 | 25.9 | 25.9 |
| Vivi KO motion | 41.7 | 41.2 | | |
| Zidane Trance | 103.3 | 58.9 | 48.8 | 45.7 |

The scene is drawn every fourth VBlank, so the PS1 budget is 16.7 ms per frame here. The changes, each with an
identical screenshot: untextured glows keep the fast row path with the mark plane (Trance: software polygons 49 ->
10 ms); the effect VM's aligned loads and stores are single accesses (they were byte-wise library `memcpy`) and
each export's table index is cached instead of searched (Ifrit's modules make ~880 export calls a frame: effect
callbacks 57 -> 20 ms); undithered and raw semi-transparent textured rows take the table path (Ifrit's fire
composite: 16 -> 5.6 ms). Still large in every battle: `display_scale` (14-17 ms, which includes the scene's GX
submission, texture uploads and the 2D layer) and the ordering-table walk.

## The hybrid in fields (tried, not adopted)

Field 259 with the hybrid renderer (sprites as GX quads where no 2D pixel lies under them in the scene): 38.2 ms
per frame against 35.9 for software. Polygon and sprite drawing fell from about 14 ms to 2.4, but the remaining
63 software sprites a frame put most of the screen on the 8-bit 2D layer, whose palette a field's colours overflow
(nearest-colour searches: display work 11 ms). The background also showed seams between tiles and the GX
scaler's point sampling. Fields need their own layout (design 5.0 to-do): the backgrounds as DS BG layers or
textures composed once per camera position, not per-frame sprites.

## Redraw skipping (tried, not adopted)

Branch `retain-experiment`: a backend over the software GPU that defers each drawing target's primitives for a
VBlank, matches them with the previous list and redraws only the entries over changed 16x16 cells, clipped to
them. It is exact (host frames identical to plain drawing on fields 259, 450, 1663, Ark and Ifrit), but on field
259 (HWSOUND=1, window ending at 609) it cost 50.3 ms per frame against 35.4: rasterizing fell only from 13.6 to
11.4 ms, because the skipped primitives are the cheap ones while the animated background and characters are
redrawn anyway, and the bookkeeping (hashing, matching, texture-write checks) added about 17 ms. Its lists take
about 400 KB, which the 4 MB DS can't spare. Even free, it would save about 2 ms of 35. Battle UI already has
its own redraw skipping in the hybrid renderer.

## Next

* SPU (about 17 ms per frame in every mode, more than a VBlank by itself): voices about 105 cycles per voice
  sample including ADPCM decode; reverb 5.8 ms. HWSOUND=1 plays the voices on the DS's own channels instead
  (design §7.0), which brings the SPU to about 5.5 ms. The DSi's Teak DSP is not used: the target is the
  original DS.
* Battle: one scene per four VBlanks (66.7 ms PS1 budget) costs about 330 ms of DS time; polygon fill is
  most of it. The software path is about 5x short; the GX path (design §5) is the alternative.
* Game logic (decomp code, Thumb, main RAM): about 16 ms of the field frame is
  outside the instrumented categories.
* GTE: about 540 cycles per command after the ARM change. Its 10 KB of code
  doesn't fit the remaining ITCM.
