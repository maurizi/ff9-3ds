# Roadmap

Status 2026-10-31 (Azahar; no hardware measurements yet).

## Done
- 3DS build (devkitARM, libctru, citro3d), the decomp's 20 binaries and 380 effect modules native on the ARM11.
- PICA200 renderer of the PS1 primitive stream: PS1 VRAM mirrored per eye on the GPU, exact blend modes, CLUT
  texture cache with content hashing, texture windows, 24-bit movies, readbacks.
- Stereoscopic 3D: GTE depth capture for every projected vertex (sub-pixel, perspective-correct), depth of 2D scene
  layers from their ordering-table entry, adaptive convergence, floating-window edges.
- Dual screen: battle UI, dialogue and all windows, menus on the bottom screen at 1:1; the scene on the top
  (frozen under a menu). Touch: tap to move the game's cursor and select, swipe back/scroll.
- Memory cards (`.mcr`, emulator-compatible), loads from the title, automatic disc changes.
- Second core: MDEC decoded ahead; the exact SPU mixed there.
- Voice acting: optional Opus packs, one recording per dialogue line (`docs/voice-packs.md`).

## Next
1. **World map speed**: ~15 ms of game-thread time per VBlank in Azahar (was ~29; needs < 16.7 for its 20 fps,
   with margin on hardware). The generic path's
   per-primitive cost (packet decode, texture lookup, vertex building) and the software GTE vertex pass. Plan: the
   terrain blocks through a native path (port/game/world_native.c's hook): faces emitted from the vertex pass's
   arrays without packets, batched by texture; then the vertex transform itself in the vertex shader.
2. Hardware validation on Old and New 3DS: frame times, NDSP, SD read speed, sleep/HOME behaviour.
3. Field bottom screen when no window is open: location, party HP, Menu / Cards buttons.
4. Native model renderers (battle and field models through the hooks the DS port found), with a depth buffer:
   prerequisites for cel-shading and replacement models.
5. Extras: cel-shading (New 3DS), high-resolution models, StreetPass (Tetra Master card exchange). Voice acting
   is in (`docs/voice-packs.md`); a tool listing every line's text by id would help pack makers.
6. CIA build with a banner; settings screen (stereo strength, filtering, widescreen).
