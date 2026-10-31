# ff9-3ds

A native Nintendo 3DS port of Final Fantasy IX (PS1, NTSC-U), built from the C source of the
[ff9-decomp](../ff9-decomp) matching decompilation. The game's own code runs natively on the 3DS's ARM11;
the PICA200 GPU draws it, in stereoscopic 3D; its menus, battle commands and dialogue are on the touch screen.

This repository contains no game data. Building and running it both require your own copy of the four NTSC-U
discs (SLUS-01251/01295/01296/01297). **A built `.3dsx` contains code only, but the data folder you put on the SD
card holds Square Enix data: never distribute it.** Final Fantasy IX © Square Enix; this project is not affiliated
with or endorsed by Square Enix.

## What it does

| | |
|---|---|
| Rendering | The PICA200 draws every scene, with the PS1's four blend modes exactly; sub-pixel vertex precision and perspective-correct textures where the GTE projected the geometry |
| Stereoscopic 3D | Every projected vertex gets the parallax of its real depth; 2D scene layers (field backgrounds) get the depth of their place in the game's depth sorting. The 3D slider sets the strength |
| Dual screen | The top screen shows the scene. The bottom screen shows, at the PS1's own pixel size, the battle commands and party status, dialogue windows, menus and the save screen |
| Touch | Tap an entry to move the game's cursor there and select it; swipe left to go back; swipe up or down to scroll a list. Every button works too |
| Saves | Two memory cards as standard `.mcr` images on the SD card: saves move to and from PS1 emulators (and real cards, via dumps) |
| Discs | All four discs are on the SD card; disc changes happen at the game's own "change disc" screen |
| Movies | Decoded on the second CPU core, ahead of display |
| Sound | The PS1 SPU, exactly (24 voices, Gaussian interpolation, reverb), mixed on the second CPU core |
| Voices | Optional voice packs: a recording per dialogue line, from the SD card ([docs/voice-packs.md](docs/voice-packs.md)) |
| Models | Old 3DS and New 3DS (the New 3DS's faster CPU is used when present) |

## Controls

| 3DS | PS1 | |
|---|---|---|
| A | ✕ | confirm |
| B | ○ | cancel; hold to walk |
| X | △ | menu |
| Y | □ | |
| L / R | L1 / R1 | |
| ZL / ZR, or L+R | L2 / R2 | L+R together: flee |
| Start / Select | Start / Select | |
| Circle pad / D-pad | D-pad | |

## Building

Requires Docker, Python 3, and raw 2352-byte images of the four discs at `disks/us/raw/FF9 CD1.img` …
`FF9 CD4.img` (the same layout as ff9-decomp).

```
git submodule update --init          # decomp/ (ff9-decomp)
make extract                         # the decomp's extract + split, on your discs
tools/docker.sh make -j16            # -> build/ff9.3dsx
tools/docker.sh make -j16 pc         # -> build/pc/ff9ds (32-bit Linux host build, for development)
```

`tools/docker.sh` runs the devkitARM container (libctru, citro3d) with Python and a 32-bit host gcc.

## Installing

On the SD card:

```
/3ds/ff9.3dsx                         (from build/)
/3ds/ff9/raw/FF9 CD1.img … FF9 CD4.img (your disc images)
/3ds/ff9/cards/                       (created on the first save; card1.mcr, card2.mcr)
/3ds/dspfirm.cdc                      (the 3DS's DSP firmware, dumped with DSP1: needed for sound)
```

Start it from the Homebrew Launcher. To use existing saves, copy a PS1 memory card image (`.mcr`/`.mcd`, 128 KB)
to `/3ds/ff9/cards/card1.mcr`.

## Development

| doc | contents |
|---|---|
| [docs/design.md](docs/design.md) | the 3DS architecture: renderer, stereo, screens, sound, threads |
| [docs/porting-notes.md](docs/porting-notes.md) | PS1 behaviours the decomp's C relies on, and how the port handles each |
| [docs/scenario-testing.md](docs/scenario-testing.md) | launching fields, battles and the world map directly |
| [docs/voice-packs.md](docs/voice-packs.md) | the voice pack format, and finding a line's id |
| [docs/decomp-requests.md](docs/decomp-requests.md) | what the port needs identified on the decomp side |

Testing in the Azahar emulator: `tools/azahar.sh` boots a build on a private X display and captures screenshots
and the game's log (`sdmc:/3ds/ff9/log.txt`); `make 3ds SCENARIO=sdmc:/3ds/ff9/scenarios/X.cfg PROFILE=1` boots
into a fixture with per-category timers; `DUMP_AT=N` writes the GPU's VRAM mirror and textures to the SD card;
`AUTOPRESS=`/`AUTOTOUCH=` script buttons and taps.

Licence: port code GPL (as ff9-ds); tools CC0 like the decomp's.
