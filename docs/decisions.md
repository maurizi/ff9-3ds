# Open decisions

Each has a default that the design assumes until it's decided otherwise.

| # | decision | default | trade-off |
|---|---|---|---|
| D1 | target hardware | **revised 2026-10-16: design toward original DS mode**, subject to measured RAM and CPU feasibility; DSi remains the working baseline | 4 MiB / 67 MHz vs 16 MiB / 134 MHz. Requires bounded buffers and native code/data overlays, not just asset streaming. Preserve graphics and sound semantics; see `ds-mode.md`. Original DS support is not implemented yet |
| D2 | where field dialogue goes | top screen, in place, ×0.8 AA font; fall back to bottom screen at 1:1 if the M5 prototype isn't legible | positional context vs legibility |
| D3 | bottom screen during FMV | black | black vs chapter art / location card (art would have to come from the disc too) |
| D4 | licence of port code | **decided 2026-10-11: GPL** (GPL-3.0-or-later unless reused code needs GPL-2.0-only); emulator GTE/SPU code may be reused with attribution | permissive (MIT/CC0, like the decomp's tools) means writing GTE/SPU from psx-spx; GPL would allow reusing emulator code (Beetle/PCSX GTE, SPU ADSR/reverb) |
| D5 | scaling | uniform ×0.8 with letterbox in 224-line modes | vs ×0.857 vertical (7% squash, no bars) or cropping with camera follow |
| D6 | how the decomp is consumed | git submodule pinned per port release | vs a path to the live checkout (faster, less reproducible) |
| D7 | equivalent-C location | `src_equiv/` in the decomp repo | vs keeping it in the port repo (no decomp-repo churn, but drifts from the headers) |
