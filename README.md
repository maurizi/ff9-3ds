# ff9-3ds

A native Nintendo 3DS port of Final Fantasy IX (PS1, NTSC-U), built from the C source of the
[ff9-decomp](https://github.com/maurizi/ff9-decomp) matching decompilation.

The game runs natively on the 3DS's ARM11 without emulation, and features stereoscopic 3D and a full dual-screen redesign of the UI.

[![ff9-3ds demo reel](https://img.youtube.com/vi/hRjhjAkbp20/maxresdefault.jpg)](https://www.youtube.com/watch?v=hRjhjAkbp20)

This repository contains no game data, and neither do its releases. To play, you need your own copy of the four
NTSC-U discs (SLUS-01251/01295/01296/01297).

Final Fantasy IX © is owned by Square Enix; this project is not affiliated with or endorsed by Square Enix.

## Playing

Download the builder for your computer from [Releases](https://github.com/maurizi/ff9-3ds/releases), run it, and point it at your four discs: BIN/CUE, IMG/CCD or raw ISO. It checks they are the NTSC-U release, unmodified, and writes the one you choose:

- `ff9-full.cia` (about 1.7 GB): install it with a CIA installer such as FBI, then start Final Fantasy IX from the
  HOME Menu.
- `ff9-full.3dsx`: copy it to `/3ds/` on the SD card and start it from the Homebrew Launcher.


## Building

The program needs no disc to build:

```
git submodule update --init          # decomp/ (ff9-decomp)
tools/docker.sh make -j16 payload    # -> build/payload/: ff9.3dsx, ff9.cia, linkinfo.json (what the builder bundles)
python3 builder/package.py --payload build/payload   # -> dist/ff9-3ds-builder-<os> (builder/requirements.txt)
```

To build and run the game from a checkout, with raw 2352-byte images of the four discs at
`disks/us/raw/FF9 CD1.img` … `FF9 CD4.img` (the same layout as ff9-decomp):

```
make extract                         # the decomp's extract + split, on your discs
tools/docker.sh make -j16            # -> build/ff9.3dsx and build/psxrel.bin (made from your discs)
tools/docker.sh make -j16 cia-full   # -> build/ff9-full.cia (the game and your discs in one file)
tools/docker.sh make -j16 3ds-full   # -> build/ff9-full.3dsx (the same, for the Homebrew Launcher)
tools/docker.sh make -j16 pc         # -> build/pc/ff9ds (32-bit Linux host build, for development)
```

`tools/docker.sh` runs the devkitARM container (libctru, citro3d) with Python and a 32-bit host gcc. The program
holds none of the game's bytes: it reads them from the discs, and `psxrel.bin` (tools/gen_relocs.py) says which
words are pointers. `tools/check_nodata.py FILE...` confirms a build carries no game data.

`build/ff9.3dsx` alone also runs with the discs on the SD card: `/3ds/ff9.3dsx`, `/3ds/ff9/raw/FF9 CD1.img` …
`FF9 CD4.img`, and `/3ds/ff9/psxrel.bin` (from `build/`).

## Development

| doc                                                  | contents                                                               |
|------------------------------------------------------|------------------------------------------------------------------------|
| [docs/design.md](docs/design.md)                     | the 3DS architecture: renderer, stereo, screens, sound, threads        |
| [docs/porting-notes.md](docs/porting-notes.md)       | PS1 behaviours the decomp's C relies on, and how the port handles each |
| [docs/scenario-testing.md](docs/scenario-testing.md) | launching fields, battles and the world map directly                   |
| [docs/ui-design.md](docs/ui-design.md)               | the dual-screen UI design, per mode                                    |
| [docs/voice-packs.md](docs/voice-packs.md)           | the voice pack format, and finding a line's id                         |
| [docs/decomp-requests.md](docs/decomp-requests.md)   | what the port needs identified on the decomp side                      |

Testing in the Azahar emulator: `tools/azahar.sh` boots a build on a private X display and captures screenshots
and the game's log (`sdmc:/3ds/ff9/log.txt`); `make 3ds SCENARIO=sdmc:/3ds/ff9/scenarios/X.cfg PROFILE=1` boots
into a fixture with per-category timers; `DUMP_AT=N` writes the GPU's VRAM mirror and textures to the SD card;
`AUTOPRESS=`/`AUTOTOUCH=` script buttons and taps.

Licence: port code GPL-3.0-or-later ([LICENSE](LICENSE)); tools CC0.
