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

The first column is from a different window of the same scene (not yet
fixed-frame), so compare it loosely. Presentation waits for VBlank, so a frame
costs a whole number of VBlanks. About 4 today; the PSX field runs at 30 fps (2).

What changed: background tiles (16x16, 8-bit, unmodulated) took the per-pixel
generic path. They now use a direct-CLUT sprite path and a row loop that reads
each source halfword once. The SPU mixes voices in exact blocks
(`audio-validation.md`). The GTE is compiled as ARM, since Thumb has no long
multiply.

## Intro FMV (title), ms per 60 Hz frame

175 at first (movie decode uninstrumented), 47 after the MDEC, display-skip and
SPU work. See `movie-validation.md` and `audio-validation.md`.

## Next

* SPU: event samples (block boundaries) and the reverb tail, about 1,000
  cycles per output sample.
* Game logic (decomp code, Thumb, main RAM): about 16 ms of the field frame is
  outside the instrumented categories.
* GPU: polygon fill; GP0 decode overhead per command.
* GTE: about 540 cycles per command after the ARM change. Its 10 KB of code
  doesn't fit the remaining ITCM.
