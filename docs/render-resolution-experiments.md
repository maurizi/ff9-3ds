# DS resolution experiments, 2026-10-15

Direct screen-resolution rendering has been measured on the native DS build.
The current production renderer retains the verified full-resolution software
path: the tested alternatives either damage the image or provide modest savings
with unresolved framebuffer dependencies.

## Matched Ark measurements

All runs boot the same disc-3 Ark fixture, use software rendering, ITCM guards
and profiling, and stop after presenting frame 1663. Each timing block covers
128 frames ending at 1633. The stop's subsequent idle time is excluded. These
are instrumented guest timings, including VBlank quantization, rather than
uninstrumented gameplay FPS measurements.

| Renderer | Interval, ms/frame | Change from baseline | Image result at frame 1663 |
| --- | ---: | ---: | --- |
| Production baseline | 116.4837 | — | Reference |
| Scale screen geometry to DS resolution | 107.8649 | 7.4% less | Text and framebuffer copies damaged |
| Native scene with original-resolution lower HUD | 115.5695 | 0.8% less | HUD exact; 11,344 scene pixels differ |
| Skip undisplayed scene samples | 112.3049 | 3.6% less | Entire active bitmap exact |
| Packed RGB15 blend arithmetic, full resolution | 116.8754 | 0.3% more | Both source and bitmap exact |

The baseline spends 55.8039 ms in polygons, 9.9829 ms in rectangles, and
9.8769 ms in display conversion. Its full GP0 processing costs 70.4410 ms.
These categories overlap; they must not be summed as independent costs.

## Direct geometry scaling

The first prototype transforms screen vertices and drawing-area corners by
4/5 before rasterization, keeping the two framebuffer origins at X=0 and 320.
Offscreen drawing stays at its original resolution. Screen rectangles become
textured polygons so their UV interpolation still traverses the original source
extent. Presentation copies the resulting 256-wide image directly.

This reduces pixel work, but replacing fast sprites with polygons adds work.
The combined polygon phase becomes 58.2343 ms; its call count includes the
converted rectangles. The simple halfword presentation loop also takes
13.3097 ms, exceeding the production scaler's paired-store implementation.
These are implementation costs, so 7.4% is not a fundamental limit on a more
extensive native renderer.

Framebuffer coordinates are also part of the game's texture workflow. The
original Ark command trace repeatedly copies 16x16 and 32x48 areas from screen
Y=154..217 into texture-cache locations, plus narrow strips at Y=157. A smaller
screen image does not satisfy those original source coordinates. The prototype
visibly corrupts text; preserving only the offscreen render-target resolution
does not address this dependency.

## Preserving the HUD

A second diagnostic renders the top scene directly into 256x100 pixels and
retains original source rows 125..217 for the lower image, including the HUD.
Screen polygons render the necessary regions in both coordinate systems;
original interpolation origins are preserved for the high-resolution region.
HUD rectangles retain the fast original sprite path. Presentation uses paired
native copies above row 100 and the existing text-aware scaler below it.

All lower 74 bitmap rows match the baseline exactly, including text. The upper
scene still differs because rounded vertex coordinates change edge coverage
and UV interpolation. Additional polygon setup and the retained high-resolution
work consume most of the saving: 54.1193 ms in polygons and 9.7786 ms in
presentation. The fixed boundary is specific to this experiment; it does not
handle arbitrary field dialogue, upper-screen battle captions or other layouts.

## Keeping PSX interpolation and skipping samples

The third prototype keeps original coordinates, edges and fixed-point
interpolation. For screen polygons above source row 125, it skips every fifth
source row and omits shading every fifth source column: those pixels are not
read by the scene's existing nearest sampling. Rectangles, the lower image,
offscreen targets and explicit shadow-region rendering keep their full paths.

Polygon time drops to 51.1095 ms. The active DS bitmap has zero differing
pixels, while the original-resolution source contains 14,099 differing pixels
at the captured frame. This proves the tested display result, not that discarded
pixels are unnecessary to every consumer. Text area filtering above the fixed
boundary needs neighbouring pixels, and framebuffer reads can need any source
pixel. A production version needs those dependencies resolved before omitting
work. The per-pixel sample-selection overhead also limits this simple prototype.

## Blend arithmetic check

A separate trial computes the four RGB15 blend modes with packed red/blue and
green lanes. Independent scalar checks cover all channel pairs at boundaries,
STP inputs, and one million mixed colour pairs; the 4,096-primitive full-VRAM
comparison also passes. The native Ark source and bitmap are both exact, and
five host battle fixtures pass 65,000 frames. Its measured interval offers no
improvement, so the trial was discarded.

## Artifacts and follow-up

All captured frames are exactly 1663; guard inspection reports no protected
ITCM changes or recorded ITCM faults. Matching ROMs, ELFs, states, profile JSON,
prototype patches and bitmap PNGs are retained under ignored `build/validation/`:

- `ff9ds_native_resolution_{baseline,prototype,hybrid,samples}.{nds,elf}`
- `ds_native_resolution_{baseline,prototype,hybrid,samples}.ml8`
- `ds_native_resolution_*_profile.json` and `*_inspect.json`
- `ds_native_resolution_samples_quality.json`
- `native_resolution_{prototype,hybrid,samples}.patch`
- `native_resolution_ark_vram_dependencies.log`
- `ff9ds_packed_blend_ark.{nds,elf}`, `ds_packed_blend_ark*.json`, and
  `packed_blend_trial.patch`

The one-second `*_stop.mp4` files record terminal diagnostic frames; they are
not animation validation. Existing actual Ark/PSX combat clips remain the motion
references. Ordinary source and builds are restored after the experiments, and
the emulator is closed.

Further native-resolution work should preserve original framebuffer consumers
through explicit render-target and copy handling, use a dedicated scaled sprite
kernel, and give text a native layout or complete source coverage. The measured
gain does not yet justify replacing the current image-quality baseline.
