# CLAUDE.md

FF9 (PS1 NTSC-U) port to Nintendo 3DS (forked from the ff9-ds DS port), built from the ff9-decomp C source in the
`decomp/` submodule. Design is in `docs/`; read `docs/design.md` and `docs/ui-design.md` first.

## Rules
- Never commit or embed game data: no disc contents, extracted blobs, generated data, converted FMV/fonts/BGs,
  built `.3dsx`. Everything derived from the disc goes in `build/` or `ff9ds/` (both gitignored).
- Never edit `decomp/` sources. Change behaviour through the prelude, PsyQ replacements or link-time overrides
  (`config/overrides.txt`), in that order (design §3.1). Non-portable decomp code goes to the decomp side as a
  request in `docs/decomp-requests.md`. Such a change must keep the PS1 `make verify` OK.
- Anything the port needs identified in the decomp goes in `docs/decomp-requests.md` (R#), not guessed here.

## Working
- Build both targets before committing: `tools/docker.sh make -j16 3ds pc` (devkitARM is gcc 16, the host is
  gcc 13: they don't reject the same things).
- `build/ff9.3dsx` is always the player build; any test option (AUTOPRESS, SCENARIO, PROFILE, dumps) writes
  `build/ff9-test.3dsx`. Before saying something works, run the real player path (title -> New Game / Continue).
- Emulator testing: `tools/azahar.sh`, `tools/render_diff.py`, `tools/smoke3ds.sh` (docs/design.md section 9).
- The PC host build is the fast loop (seconds per run, gdb). Recipes are in `docs/porting-notes.md`. Its options:
  `--frames N --status N --watch ADDR:SIZE --press BUTTON@FRAME[+N] --render --dump N`. Without `--render` or
  `--dump` the host does not rasterise at all, so rasterizer bugs (and costs) only show with one of them.
- A crash in decomp code is usually one of the classes in `docs/porting-notes.md`. Fix it with the matching
  mechanism (counted patch in `config/patches.txt`, PsyQ layer, override), and tell the decomp session when the
  source can say it more clearly without changing the PS1 output.
- The decomp is a submodule; pull its latest with `git -C decomp fetch local && git -C decomp checkout local/main`.
  A patch that stops applying fails the build: that usually means the decomp fixed the site, so drop the patch.
- After pulling, regenerate its splat output (`cd decomp && tools/docker.sh make split`; asm only, the C is kept)
  and rebuild. The overlay trampolines and the MIPS comparison tests read `decomp/asm/` by function name, so stale
  asm after a rename links calls into raw MIPS bytes (a crash at a `psx_ram` address). Names the decomp gives to
  functions the port also uses must be renamed on the port side too, including the `oXX_`-prefixed overlay forms.
