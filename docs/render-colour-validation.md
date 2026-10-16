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
