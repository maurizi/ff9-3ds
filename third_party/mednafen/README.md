# Mednafen MDEC arithmetic reference

The precision adjustments, signed-nine-bit clamp and colour conversion in
`port/psx/mdec_decode.c` are adapted from the Mednafen PSX module's `mdec.cpp`:

* Copyright (C) 2011–2016 Mednafen Team.
* GPL-2.0-or-later; the upstream licence is included in `COPYING`.
* Source revision: `f0ee9d595db68ad5247ba5ac6a8367fdced9c3fc`.
* [Original source](https://github.com/libretro-mirrors/mednafen-git/blob/f0ee9d595db68ad5247ba5ac6a8367fdced9c3fc/src/psx/mdec.cpp).

The port uses a standalone, bounded C interface for one macroblock, no emulator
state machine, and scalar matrix passes over the caller's raw scale table.
It does not incorporate DuckStation source code. Emulator comparisons and
remaining accuracy limits are documented in `docs/movie-validation.md`.
