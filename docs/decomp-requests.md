# Requests to the decomp side

What the port needs from `ff9-decomp`, in priority order. **P0 blocks the first ARM build, P1 blocks rendering
and audio bring-up, P2 blocks the native dual-screen UI, and P3 is optimisation.**

General rules for the decomp agent:
- Everything here can be delivered without breaking `make verify`. Port-only C (functionally equivalent,
  non-matching) goes in a separate tree that the PS1 build never compiles. `src_equiv/<binary>/` is proposed.
- Names follow `docs/naming.md`, with evidence tags as usual. For the port, **identifying the boundary**
  (entry point, state struct, input read) matters more than naming every helper inside it.
- Runtime work goes through the game-tester workflow (one emulator at a time). Items marked 🎮 need the emulator.

---

## P0: needed to build and boot

### R1. Equivalent C for every remaining asm routine
The port has no MIPS. Every routine below needs a C body that behaves the same; it doesn't have to match. Where a
routine uses a custom register convention, document that convention and give it a normal C signature instead.

| area | routines | notes |
|---|---|---|
| main text/glyph module `0x80050F4C–0x80053924` | 63 (`func_80050F4C` … `func_80053764`) | global registers `$t5/$s6/$s7`, no ABI, some GTE. This is the dialogue/window text renderer (the opcode interpreter at main.c:~14140 calls `func_80050F4C(colour)` / `func_800512C8` / `func_800512B4`). **Highest value**: the port's native text renderer (R15) needs its semantics anyway |
| `text_setBank`, `text_get`, `text_getInfo`, `text_getFrom` | 4 | trivial |
| sin/cos/rand module `func_8004BB7C`–`func_8004BF74` (with `rand_refill`) | 7 | fall-through entry points; cos/sin table lookups |
| `func_80064C24` | 1 | calls `func_800512E4` with the register ABI |
| `func_80062C8C` (libgte hand-written) | 1 | name it if it's a known libgte routine; the port provides libgte |
| ovl_0a | `func_800C1DC4/DE0/DF4/E04`, `func_800B5958`, `func_800C9AB8` | |
| ovl_0b | 47, including the `CF5xx–CFAxx` / `DFE4C–E0014` blocks | battle |
| ovl_0c | 16 (`func_800B3844` … `func_800B6598`, `func_800CB1EC`, `func_800C709C`) | world map, probably terrain/GTE inner loops |
| the 16 non-matching game functions | main `8002E718 800475D8 8004DAD0 8005681C 80059414`; ovl_0a `800B4A5C 800BD130 800C0038 800C07D8 800C65CC`; ovl_0b `800E4A3C 800BFF20 800C3F78 800DA138`; ovl_0d `800AC708`; ovl_37 `801F13F8` | drafts in `docs/wip/`: say which ones are **behaviourally** correct (register allocation only) so the port can use them now |

Please also confirm that no *game* code is hiding in `config/library_funcs.us.txt` apart from the above
(`func_80064C24` "out of scope" for example).

> **ff9-decomp, partial (commit 824d1e6 and earlier):**
> - done: main text/glyph module `0x80050F4C–0x80053924`, all 63 routines, in `src_equiv/main/script_text.c`.
>   It's not only text: it is the pen/glyph emitter, the debug printf, the **event-script expression evaluator**
>   (`func_80051F54`), variable read/write (`func_80051D4C`/`func_80051E8C`), the **script scheduler**
>   (`func_80052870`: runs each object's script, handles jumps/switches/waits itself, everything else goes to
>   `evt_execInstr`), geometry (atan, length, isqrt, point-in-polygon, `ratan2`) and two streak-particle pools.
>   The register convention ($t4–$t7 pen state, $t8 glyph sink, $s5/$s6/$s7 script flags/ip/stack) is
>   documented at the top of the file and turned into file-static state; every function has a normal C
>   signature matching `include/`. Math helpers were replayed against emulator captures (0 differences); the rest
>   is checked against the asm by reading.
>   One port hazard: evaluator opcodes 0x60/0x61 take an object pointer that went through the 26-bit stack and
>   restore it with `| 0x80000000` (commented in the code).
> - done: `text_setBank/text_get/text_getInfo/text_getFrom` → `src_equiv/main/text_bank.c`.
> - done: sin/cos/rand block → `src_equiv/main/math_rand.c` (`math_sin`, `math_cos`, `rand_refill`, the
>   random-byte routine). `func_8004BBBC/BBD0/BBDC` are jump targets inside `math_sin`, not functions; nothing
>   calls them. `rand_refill` was checked byte-for-byte against a real refill.
> - done: `func_80064C24` (pen colour preset) → end of `script_text.c`.
> - `func_80062C8C`: a file-local helper of libgte MSC02.OBJ (no name in the signatures), called only by
>   `VectorNormal`, `VectorNormalS`, `VectorNormalSS` and `MatrixNormal`. The port's libgte replaces those, so it
>   needs no equivalent.
> - open: the ovl_0a/0b/0c asm routines, the 16 non-matching drafts, and the library_funcs audit.

### R2. Hardware-touching game code
List every game function (outside PsyQ) that reads or writes hardware registers or DMA directly: the SPU (the AKAO
`SPU_REG16`/`SPU_VOICE_REG` users, main.c ~17300–17400 and ~19670), GPU (`GPU_cw`), CD, root counters,
interrupt masks. For each, say read or write, which registers, and why. The port reimplements these at that level.

### R3. Cross-binary pointers
The port links all overlays into one binary, each in its own namespace. It has to know which overlay every raw
address belongs to.
- **Main's mode loop** (`func_800127A0`): which overlay each entry belongs to: `func_800A7088` (mode 0),
  `func_800A7A48` (1), `func_800A859C` (2), `func_800A8860` (3), `func_800A713C` (4), `func_800A80A4` (7),
  `func_801EF284` (8), `func_800A752C` (9). Also: modes 7 and 8 both load file 101 (`0x65`) but call different
  addresses in different slots. What are modes 7 and 8?
- **Data tables** holding raw overlay addresses, e.g. `D_8006C9D4` (menu overlay init/update table: `0x801F3874`,
  `0x801F4748`, ...), indexed through `D_8006CA40` (overlay file ids `0x3A 0x3C 0x3B 0x39 0x32 0x3D` ...). File
  `0x3D` isn't in the code-overlay list: what is it?
- The port will generate candidates with `tools/find_xrefs.py`: every `.word` in any binary's data in
  0x80010000–0x80200000 that splat didn't symbolise, plus every `jal`/`la` target outside its own binary. The
  request: **confirm the ambiguous ones**. A `binary:addr → ovl` list in `config/` would be ideal.

### R4. Fixed RAM addresses
For each literal below: what the buffer is, its size and its lifetime. The port maps them into an arena, so it
has to know about overlap with linked symbols.

| address | where | |
|---|---|---|
| `0x801FCB00` (14 uses) | ovl_39 | save/load buffer? |
| `0x801E7700` (9), `0x801F1700`, `0x801EC700` | ovl_0b_2 | |
| `0x80104000`, `0x80104A00`, `0x80103800`, `0x80103C00` | ovl_0d | Tetra Master |
| `0x801CDF00` | ovl_14, ovl_3e | |
| `0x801EDF00` | main | menu overlay slot, also used as a buffer by `func_80028B20` |
| the rest | `grep -rnoE '0x80[0-9A-F]{6}' src` | 33 distinct |

Also, is **`ovl_0a.c:6423` `if (p > 0x80000000)`** a pointer-validity test? On DS, pointers are `0x02xxxxxx`.
Please check the other 15 `0x80000000` uses quickly; most look like flag bits (`statusImmune`, the RNG).

---

## P1: rendering, audio, files

### R5. Frame structure per mode 🎮
For each mode (field, battle, world, menu, Tetra Master, opening): the frame begin and end
(`gfx_selectBuffer`, the `DrawOTag`/`PutDrawEnv`/`PutDispEnv` call sites), the **display resolution**
(320×224 or 240? interlaced anywhere?), OT length, and the frame rate actually achieved (VSync interval: 30 or
20/15 in battle?).

### R7. VRAM traffic classification
Classify every `StoreImage` (34), `MoveImage` (30) and `LoadImage` (88) site, and every DR_AREA/offscreen draw,
as one of:
**(a)** loading textures or CLUTs at scene load, **(b)** per-frame texture or CLUT animation, **(c)** reading or
copying the framebuffer (battle-entry swirl, blur, fade, screenshots for the menu background), **(d)** rendering
to an offscreen area and using it as a texture. The port handles (c) and (d) with display capture, so it needs
the exact sites.

### R8. Semi-transparency usage 🎮
Count primitives per frame by blend mode (0–3) in: a normal field, a battle with spells (Fire/Thunder/Cure), a
summon (any Eidolon), Trance, and the world map. Also the subsystems that set modes 1 and 2. This decides the
scope of the 2-pass additive path.

### R9. Polygon budget measurement 🎮
Count primitives per frame by type (POLY_F3/FT3/G3/GT3/F4/FT4/G4/GT4, SPRT/SPRT_16/TILE, LINE) and the number
of `RTPS`/`RTPT` per frame, in: Alexandria main street, a dense field (Treno, Lindblum), the world map from the
airship, a 4v1 battle, a summon, Tetra Master. Hooking `DrawOTag` and walking the OT in the emulator is enough.
The numbers go in the port's `docs/perf.md`.

### R10. File, stream and disc layer
- The game's CD module API: `cd_startRequest`, `g_cdLoad`, `func_80022C60` (load(sector, size, dest,
  callback)?), `func_80022B18` (busy?), `func_8001E218` (file lookup by dir/id), `g_gameState->unk20`
  (FF9.IMG base sector?).
- **The FMV player in play** (ovl_64 is the debug one): which code plays STR files and MBG movie backgrounds, how
  an event script triggers it, and how MBG frames sync with field scripts.
- Disc-change prompt and check code.
- Any XA audio streaming outside FMV.

### R11. Sound boundary
- The full `akao_command` command set: which commands game code issues (music play/stop/fade, SFX, volume).
- How sample banks reach SPU RAM (`SpuWrite`/transfer callers, file type, and when).
- Does any game code **read** SPU registers or voice status (key status, envelope)? Read behavior matters
  for driver decisions as well as the full synthesis needed for audio fidelity.

Port-side sound-boundary findings (2026-10-16; no decomp edits): `func_800575B4`
writes KON0/KON1, `func_800575D0` writes KOF0/KOF1, `func_800575EC` writes reverb
send bits, `func_80057608` writes noise enable bits, and `func_80057624` writes
pitch-modulation bits. `func_800576BC` writes one voice's LSAX. These are candidates
for descriptive upstream names after matching verification. `func_800577EC` also
writes LSAX directly through its voice-register pointer during bulk setup, so
intercepting only `func_800576BC` would miss writes. Explicit repeated writes must
remain visible to the sound core: loop override and envelope counter resets
cannot reliably be inferred by comparing register snapshots. Key-off starts ADSR
release and must not discard ADPCM source/predictor state. The source/envelope
components under `port/psx/spu_*` are now integrated with these boundaries for
optional host dry-audio capture. The port overrides these writers plus the bulk
setup/initialization and CD-volume writers. It also declares original boundaries
`noipa` in its prelude: GCC had inlined the old KON stores into AKAO before
link-time weakening, bypassing the replacements and leaving all voices silent.
The original decomp's C and PSX build remain untouched by that workaround.

### R12. Pad layer
- The raw → logical bit mapping (`0x20000` = confirm), and where button configuration is applied.
- Per mode, where L2/R2/Select/Start/□ are read (flee, ATE, help, cards, pause) and whether analog input is used.

### R13. Memory card
The ovl_39/libcard path: confirm it only uses BIOS `_card_*` and libcard (so a file-backed 128 KB card works),
plus the save block layout boundary (one save = which blocks).

### R14. Timing
Uses of `GetRCnt`/root counters and `VSync(-1)` timestamps outside libetc and AKAO, and whether anything depends
on the exact PS1 timing.

---

## P2: dual-screen UI. For each item: (a) draw site, (b) state, (c) input read

### R15. Messages and windows
`msg_open(…, layout, …)` (layout semantics), the `Window` struct, window position and size computation, the text
opcode interpreter (main.c ~14100–14400: colours, variables, waits, icons), and **choice selection**: option
count, the cursor index variable, and how the result is returned to the event script. Also the field "!"/"?"
prompt icons and the ATE prompt.

### R16. Battle UI
`menuState` / `cmdUnit` (already found) plus: the command list per unit, sub-menus (Skill, Item, Summon lists),
the target cursor and `targets` bitmask selection, the ATB/HP/MP/Trance HUD draw function(s), the help window,
the battle message banner, and where the menu reads the pad.

### R17. Menu overlays
For each of `32 33 34 36 37 38 39 3a 3b 3c 3e 65` and the top-level main menu (which lives in the exe or ovl_0a,
per the runtime findings): the per-frame update and draw functions, cursor and list state, and the text banks.
`3c` and `3e` purposes are still unknown.

### R18. Field
The player movement input point (where the d-pad becomes a direction), the camera (`func_800BF660`), the BG layer
system (`func_800BE158`, `BgTile`, `BgSprSet`, `func_800BB090` animations, scroll wrap), the walkmesh (only if
tap-to-walk is ever wanted), the location-name display, and the encounter flag.

### R19. World map
The minimap draw (it's toggled by Select on PS1), the player position and heading in map cells, the vehicle state
(chocobo/airship, ascend/descend input), and the place-name lookup.

### R20. Mini-games and event-script input
The event script opcodes that read the pad (`evt_execInstr` opcode table), so on-screen buttons can feed them:
jump rope, the Hippaul race, Chocobo Hot & Cold digging, Mognet deliveries.

### R21. Tetra Master and Blackjack
ovl_0d: board state, both hands, the selected card and cell, turn phase, where input is read. ovl_15: the action
selection.

---

## P3: performance

### R22. Model formats and renderers
The field and battle model formats (vertices, bones, primitive lists), the model draw functions (ovl_0a around
ovl_0a.c:5430/6144, the battle equivalent, ovl_0c), and the animation format. This enables converting models to
DS display lists with hardware transform.

### R23. Battle effect modules as C (2026-10-17)
Done (2026-10-24): all 380 modules are matched C and native in the port; the interpreter is removed.
The CD-loaded effect modules (loaded at 0x801E7700, header pointer there, callbacks into the ovl_0b export table
`g_btlfxModuleExports`) are the one body of game code the port still interprets: `port/game/effect_vm.c` runs their
MIPS. With export calls cached and aligned memory accesses fixed, the interpreter still costs about 100 ARM9
cycles per guest instruction: 8.6 ms of a 60 ms frame in Ark and 20 ms in Ifrit's summon (18K guest instructions
and ~880 export calls per frame). Native code for them would remove nearly all of that.

Asked: (a) the list of effect module files on the disc and their sizes; (b) whether the decomp can split and match
them like the overlays (one binary per module, all linked at 0x801E7700), even as `src_equiv` C that only needs
to behave the same. The port would then compile them like the overlays and load the native code with the module.
If (b) is out of scope, the port can instead translate MIPS to ARM when a module loads, and (a) is enough.

### R29. Facts for the speed work on the original DS (2026-10-30)
The original DS must reach 16.7 ms per PS1 VBlank (battles are ~25 now, fields median 20.5, world map 25-29). The
port's ideas need facts only the decomp can give; each item says what it would de-risk. Answers in prose are fine.
1. **Is battle model data immutable after init?** After `btl_applyModelTextures` (btl_mesh.c, uv offsets applied
   once under `h->flags & 1`), does anything rewrite a `BtlMesh`'s `prims`, `uvs`, `colors` or `vertexCounts`?
   Candidates: `btl_setModelRgb`/`btl_fillModelRgb`/`btl_addMeshRgb`/`btl_setMeshRgb` (and whether
   `btl_setUnitBrightness`/`btl_updateUnitTint` call them every frame), `btlfx_splitMeshVertices` (hi_vertex.c),
   `btlfx_projectMorphMesh`, Trance/petrify/zombie looks, fx modules. Which paths change a polygon's flags (its
   translucency: fades) or texture index, or a model's texture page/CLUT words per frame (`btl_makeEnemyGreyCluts`,
   `btl_saveEnemyCluts`, `texAnims`)? Is any mesh drawn double-sided (no NCLIP)? (The port now caches each mesh's
   texture bounds across scenes.)
2. **Battle OT rules.** How `btl_drawMesh` computes each polygon's OT index (AVSZ, `otBias` semantics,
   `btlbg_setOtShift`'s shift, the clamp); the link order per scene (units in `unitList` order, then `btlbg_draw`,
   banner, effects: all LIFO at the entry head?); whether effects (`g_btlfxEnv->prim`) or shadows
   (`btl_drawShadow`) rely on sharing an entry with a model polygon. (A single model pass with a fixed depth rule
   per entry, ~12% of a battle.)
3. **When a battle arena is still.** Per background id: its scroll count, texture animations, animated groups
   (`btlbg_draw`'s per-id cases and the default rotation of groups 1+), sky speed, layers used, so the port knows
   which arenas are static. `btlbg_drawRecord`: a record's depth rule (fixed entry or projected) and whether a record
   can sit in front of units (pillars, Evil Forest trees). What `g_btlbgSkyOwnProj` changes; which effects draw the
   background themselves (btlfx_op.c ~1200). (Reusing the arena as a captured image while the camera holds.)
4. **Battle frame structure.** What `gd->unk842`/`unk8C4` (btl_main.c ~2120) are (VBlanks per scene); whether
   every unit re-animates every scene (`btl_updateUnitModel`'s `a->frame++`) or bone matrices can repeat (stop,
   petrify, freeze, flag bit 21); what `btl_updatePause`'s `DrawOTag(D_800FDEB0)` replays.
5. **Field model animation rate.** fld_movie.c (~413-480) recomputes `fldmdl_calcBoneMatrices` and `worldMatrix` for
   every object every draw: which function steps `EvtObject.animFrame`, at what rate against the draw, and is there
   state saying an object's `pos`/`rot`/`animFrame`/`anim` didn't change, so the port can skip its projection when
   the camera didn't move either? (The take pass and projection are ~25% of the worst fields.)
6. **Field background layer motion.** Which code moves or animates layers (camera parallax per layer, the event
   script's tile animations, `bgi_updateAnims`/`bgi_showAnimFrame`), and any per-layer "unchanged this frame" state
   (the port hashes every layer's tiles each scene, ~1.5 ms).
7. **World map lighting constancy.** When `g_worldMapLightMatrix`, the colour matrix and back colour change (weather,
   time of day, `wm_effect.c` block effects), whether block vertex normals are static, and what per-frame inputs feed
   the depth cue and fog: a per-block lit-colour cache (the vertex pass is ~15% of a world map frame).
8. **ovl_0b's memory map with lifetimes.** Its .bss/.data buffers with size and lifetime: load-only staging
   (`btl_load*Step`, `btl_allocBuffers`), per battle, per frame, and dead ranges, and the double-buffered
   `work->prims`/`ot` sizes against their worst real use. (Room for caches: the battle heap has ~3 KB free.)
9. **Battle bone/vertex layout.** Are vertices contiguous per bone in `vertexCounts` order (as `btl_projectMesh`
   reads them), and how often do polygons span bones? (Per-bone normal-cone culling at load.)

### R28. Field model files: format, which fields load which, and which are the party (2026-10-29)
On the DS the field models are the largest single cost of the slowest fields (median 8.5 ms per PS1 VBlank of the
15 slowest sampled fields, the whole gap to PS1 speed in five: docs/ds-mode.md, "What the field models cost"). The
user has approved simplifying non-party field models at build time, as the port does for the heaviest battle
enemies (R26/R27, tools/simplify_battle_models.py: sector patches the port's CD layer serves, outputs never
committed). The party (Zidane, Vivi, Garnet/Dagger, Steiner, Freya, Quina, Eiko, Amarant, in any of their field
forms) stays exact. Asked, as R26 did for battle models:
- **Where a field model comes from**: the FF9.IMG directory and key of a field model file, and how a field's
  actor (or `ModelObj`, `fldmdl_initModel`) names it, so a tool can list each field's models from the disc.
- **The file's layout as loaded**: the `ModelObj`/`WmModelGroup` header (counts at +2, otBias +0x13, bone vertex
  counts +0x14, vertices +0x18, polygon sources +0x1C as the port reads them), the eight polygon lists (FT4 FT3 GT4
  GT3 G4 G3 F4 F3: their source records and the packets `fldmdl_buildPackets` makes from them), uvs, colours,
  texture words, what is relocated at load, and the motion format if it differs from battle models' (R27).
- **What else indexes a field model's vertices or polygons by number**: e.g. a vertex used as an anchor (speech
  balloons, held items, effects, look-at), `fldmdl_tintGroup`/`setSemiTrans`/`setAbr` (per group, fine), texture
  animations (`fldmdl_updateTexAnims`: by polygon?), shadows, mirrored or clipped draws.
- **The party's model ids** (every field form, including costumes and the Moogle-suit/disguise variants if any).
- Python in tools/ (as battle_models.py) listing every field model with its counts and the fields that use it.

: a model's bone transforms from its motions (2026-10-29)
After R26 (thanks: docs/battle-models.md, tools/battle_models.py, tools/battle_budget.py), the port draws battle
models' triangles as strips and simplifies the heaviest enemies at build time (tools/simplify_battle_models.py,
config/model_patches.txt; only vertices whose triangles all sit on one bone collapse, since vertex positions are
bone-local). Grand Dragon (model 355) and Red Dragon (610) barely simplify (2-8%): most of their vertices lie in
triangles that span bones. Asked, to judge those collapses in one frame:
- **The motion key format** (items 0x03xxxx in the model file, one key per bone per frame, read by
  `btl_calcBoneMatrices`, asm; `src_equiv/ovl_0b/bones.c`): what each key holds (rotation angles and their order and
  units, any translation or scale), and how a bone's matrix follows from its parent's, `BtlBoneDef.length` and the
  key (the order of the rotations and the z offset), so a script can pose the model.
- Which motion and frame is the enemy's idle stance (`motionIds[0]`, frame 0?), to pose it as it is mostly seen.
- Python in tools/ (as battle_models.py) that returns each vertex's model-space position for a given motion and
  frame would be ideal.

### R26. Battle model files: their format and which encounters use which (2026-10-29)
DS battles past the 3D engine's 6144 vertices now draw in two passes (exact, but 15-21 ms per VBlank slower). The
user chose to simplify the few models that tip a battle over, at build time, from the disc's files (outputs only in
the port's build data, never committed). A survey of 18 battles: the party (~2,400 front-facing vertices) and the
arena (~2,300) are about the same everywhere; the enemy decides it. Over budget: Hecteyes (1,276 front-facing
polygons), Ochu (1,066), Bandersnatch (1,041), Grand Dragon (974), Wyerd (925), Abadon, Adamantoise, Whale
Zombie, Zaghnol, Clipper. Asked, to make the list complete from the disc and to write the simplifier:
- **Which model file each enemy type loads**: the field of the encounter's enemy records (`unk20[6]`, "file ids",
  in the record `btl_initEnemyType` reads) that holds it, and how a file id maps to its FF9.IMG directory and
  entry. With it, `tools/scenario_catalog.py` can list every encounter's enemy models and their polygon counts.
- **The battle model file's layout** as loaded (`BtlModel`/`BtlUnit9C8` header, `BtlMesh` groups, per-bone vertex
  counts, the eight polygon lists, uvs, colours, texture words), including what is relocated at load (unk0 bit 1)
  and any data the motions or effects index by polygon or vertex number. A simplified file must keep every index
  the game or its effects use (bones, hidden-mesh bits, effect anchors) valid.
- Whether anything other than `btl_drawMesh` reads a model's polygon lists (effects that draw a model's polygons
  themselves, `hi_model.c`'s callers, shadows, Trance or petrify recolours).

### R25. The last code without C or a port equivalent (2026-10-24)
**Done** (decomp 1e155f14): C for all 11 world-map routines (the world map runs on the host), the two cross-binary
calls named, the 24 main routines audited as unreachable PsyQ internals (they stay link stubs), C for ovl_0b's 46
hand-written routines (the port's own `battle_*.c` versions are retired: 270 host frames over nine battles identical).
With every effect module native, the port interprets no MIPS. What its link still stubs (calling one stops with
its name; `build/ds/stubs.txt`):

- **ovl_0c, 11 routines with neither C nor a port version**: `func_800B3FEC`, `800B4270`, `800B4720`, `800B4BDC`,
  `800B4FD4`, `800B53FC`, `800B5880`, `800B5CD0`, `800B5FD0`, `800B6338`, `800B6598` (`func_800B3FEC` is called
  from ovl_0c.c:5308 with the ordering table: the world map's terrain/sea drawing?). The world map can't run without
  them. **Highest priority.**
- **Two cross-binary calls from main with no resolved target**: `func_800A80A4` (main.c:42) and `func_800B73F8`
  (main.c:12155, from the event-script handler). Which overlay defines each, and is it already in C there?
- **main, 26 routines** (`func_8001455C` … `func_800164CC`, `func_8001BE14` … `func_8001C0FC`, `_padIntPad`,
  `_padIsVsync`): PsyQ library internals, which the port replaces wholesale. Please confirm none is called from
  game code.
- Lower priority, to retire port-written equivalents: C for the rest of the asm in ovl_0b (46: the port's
  `battle_*.c` reimplement them, checked against the MIPS), ovl_0a (6, `src_equiv`), the `src_equiv/` routines in
  main/ovl_0a/ovl_0c, and ovl_64 (the debug FMV player, 21; unused by the game?).

### R24. Which fields start which encounters, and with which background (2026-10-18)
The port's direct-entry battle sweep (porting-notes "Battle sweep") enters every encounter in the encounter
directory; 408 of 1493 can't be entered alone because their bundle has no background: the field or world map that
starts the encounter supplies it (`btlbg_init` finds nothing otherwise). Asked: where a field's (and the world
map's) encounter table and its battle background selection live (field data or event script opcodes), and their
format, so `tools/scenario_catalog.py` can list (field, encounter, background) and the sweep can start each
encounter from a field that really uses it.

### R30. A field's exits, and whether an Active Time Event is available (2026-10-31)
The 3DS port's bottom screen in a field (port/game/field_hud.c, field_map.c) draws the walkmesh from above with the
player on it. Asked, to complete it: (1) where a field's exits live (the event script's region objects that change
field: their trigger polygon or line on the walkmesh, and the field each leads to), so the map can mark them; (2)
what makes the PS1 show the "Active Time Event" prompt (the flag or script variable that says an ATE can be viewed
with Select now), so the bottom screen can offer it as a button.
**Answered 2026-10-31** (ff9-decomp docs/r32/3ds-requests.md, commit 7966d2f5): exits are region objects (op 08) whose
tag-0 function sets the polygon (op 29) and tag-2 function goes to the field (op 2B / B6); the ATE is viewable when
the global byte b[0xEE] (0x8007AFDE) is 1 (op D7 / g_atePromptMode draws the prompt). Button sprites, the main menu's
cursor and opening it onto a screen: same document.

### R31. What the main menu's screens point at, for the top screen (2026-10-31)
The 3DS port can now draw onto the top screen with the game's window and text code (port/game/field_hud.c top_hud,
render.c ctr_top_ui). Its detail views need, per menu screen: the highlighted item (Item, Shop), the slot and
candidate (Equip, and how the before/after stats are computed), the highlighted ability and its AP (Ability), the
highlighted save block and its summary (Save), the letter (Mognet). Asked of the decomp session 2026-10-31.

### R32. Which field character opens a shop (2026-10-31)
The field map marks shopkeepers and save moogles (port/game/field_map.c). A moogle's own script entry holds MENU
(0x75, literal type 4), but a shopkeeper's doesn't: in Dali's weapon shop (field 354) the MENU type 2 sits in entries
3 (the player's) and 7 (not instantiated), and the shopkeeper (actor, entry 11) reaches it some other way. Needed: how
a talk function hands off to the function that opens a shop (which op, its operands: entry and tag), so the port can
follow it from an actor's talk tag to the MENU, or else a per-field table of shopkeeper uids if the game has one.
**Answered** (ff9-decomp docs/r32/3ds-requests-3.md): trigger functions walked with the op table, calls followed.

### R33. Tetra Master state (2026-10-31)
Phases, cursors, confirm blocks, board/hand structs, card images and panels for a native two-screen card game.
**Answered** (ff9-decomp docs/r32/3ds-requests-4.md); used by port/game/tm_ui.c.

### R34. Mognet (2026-10-31)
The menu/overlay that runs Mognet, its state and cursors, the read/send functions, letter text, the unread/deliver
flags (a field-screen badge) and how the field knows a moogle is present. Asked of the decomp session.

### R35. 3D models for a native renderer (2026-10-31)
Field, battle and world map model formats in RAM (mesh, skeleton, animation, faces with UV/CLUT/tpage), the draw
path (bone matrices, camera, projection, the per-face emitters), how to load and pose a party member's model outside
battle for the Equip view, and how model lighting/colour is computed. Asked of the decomp session.

### R36. Story states for an ATE and the end-of-disc-1 disc change (2026-11-01)
For scenario fixtures: a field id with the story value and global vars under which an Active Time Event is offered
soon after entering (the ATE button's first real check), and the state just before the script's disc change at the
end of disc 1 (op 0xAC / menu type 8). Opening menu type 8 directly from a disc-1 field leaves the field reloading in a
loop, with the stock flow too: the script sets something around the op that a direct open lacks. Answered
(ff9-decomp docs/r32/3ds-requests-6.md): ATE at Lindblum field 552 (tests/scenarios/disc1_ate.cfg); the disc change
from field 70 with b[2..3] = 1 (tests/scenarios/disc1_disc_change.cfg). A direct open skips fld_discChangeLeave's
file_loadDirTables / fld_refindSectors, hence the loop; the real path (op AC from a field) is fine.

### R37 / R38 / R39 (2026-11-01)
R37: world-map vehicle / chocobo / moogle / Blackjack fixtures (answered: ff9-decomp docs/r32/3ds-requests-7.md;
tests/scenarios/disc3_world_*.cfg, disc3_hot_cold.cfg). R38: field minigame fixtures (asked). R39: each field's
walkmesh, exit regions, walkmesh-switching ops and NPC places read offline (answered: docs/r32/3ds-requests-9.md and
its reader tools/field_maps.py, which tools/bake_maps.py calls).

---

## Open findings from the port (queued while the decomp session was offline)

### F1 (2026-10-11). Definition/declaration mismatch that tools/void_returns.py misses
ovl_0a.c:3687 defines `void func_800B2CC0(s32, s32, s32, s32)` (it calls func_800B31B8), but ff9.h:3535 declares
`s32 func_800B2CC0(...)`, and main's script opcode 0x8F uses the result (`if (func_800B2CC0(...)) { retry }`). On
MIPS, $v0 carries func_800B31B8's result. Off MIPS the value is garbage, and the opening scene's script retries
forever. The tool trusts the caller's declaration (s32). ff9.h hides main-only declarations from overlay files,
so gcc never sees the conflict.
- (a) Fix: `s32` plus `return func_800B31B8(...)`. The port patches it at build time until then.
- (b) Extend the check to compare every definition with every declaration of the same symbol, in any binary or
  header (return type and parameter count).

### F2 (2026-10-12). Player movement reads through a NULL collision result
The draft `docs/wip/main_800475D8.c` calls `func_80038618(obj, 0, &dist)` and reads `o->unkF`
before checking `o != NULL`. Moving toward the candle in opening field 70 reaches `o == NULL`;
the PC faults at address 0x0F and the DS data-aborts. The original assembly confirms the unconditional
`lbu v0, 0xF(s3)` at 0x800478D8, followed by the NULL check at 0x800478FC. The port maps only this
read through `PSX_NZ(o)` with a counted build-time patch, preserving the subsequent NULL collision test.
This is another low-RAM read dependency, not grounds for inserting a NULL guard into matching PS1 code.
Please include this site when documenting movement/collision semantics (R18).

### F3 (2026-10-12). Battle overlay restored from cache bypasses sector relocation
Entering battle 336 restores overlay 0B with `func_80012900` from cached RAM/VRAM.
A PC watchpoint on `D_800F78A4` shows `StoreImage` restoring raw `0x800F7870`, then
`func_800E4098` faults when it uses that pointer. The sector callback did not run for
this prefix. The port now relocates the restored prefix immediately after either
cache path; later sectors retain their normal relocation. This is a port loader
requirement, not a request to change the matching PS1 loader.

### F4 (2026-10-12). Three battle initialization dependencies on low RAM
Confirmed against a running PSX New Game, after the masked Baku entrance:
- `func_800EA004`: `g_btlUi` at `0x80104F88` is NULL before allocation; the menu-state
  byte at physical `0x12B4` is zero. The port maps this query through `PSX_NZ`.
- `func_800BBD3C`: Baku's model at `0x8010AF08` has `unk50 == NULL`. The original
  writes texture-animation record 2's fields through that pointer (`0x2C`, `0x34`,
  `0x3A`). The port maps those three writes into its low-RAM shadow.
- `func_800BC78C`: `Battle.curCmd` at `0x80109888` is NULL while selecting idle
  animation. It still reads the unit pointer at physical `0x4`; on the PSX this
  contains `0x275A0C80`, which does not equal any battle unit. The port maps the
  corresponding unit/command-number reads through `PSX_NZ`.
These are original low-RAM dependencies. The port does not have a complete BIOS
kernel RAM image; its shadow preserves addressability and subsequent writes, not
all initial kernel instruction bytes. Please include these sites in R4/R16.

### R1 follow-up (2026-10-12). Battle handwritten helpers implemented in the port
`port/game/battle_math.c` supplies nine scalar entries at CF560/574/58C/690/6B8/
708/748/788/9DC. CF588 is mutable code-adjacent data, despite its function label.
CF58C stores the background OT shift at CF5A0. The RNG seed is scratchpad +0x3FC.
`battle_background.c` implements CEED4/CF448 (background packet emission and packed
vertex projection). `battle_pose.c` implements C7DAC (packed animation decoding and
bone hierarchy composition). `battle_packets.c` implements C58E4 (eight mesh
primitive formats, active OT insertion and packet-buffer end marker).
Please resolve names/data labels upstream while preserving PS1 output. The port
keeps these as strong native definitions over weak missing-function stubs.

### F5 (2026-10-12). E4A3C WIP stack-home struct is not a native calling convention
`docs/wip/ovl_0b_800E4A3C.c` gives the third argument the by-value `E4Arg`
type, whereas its header and callers pass two pointers as arguments 3 and 4.
The draft immediately assigns `b = a; c = b`, discarding its other incoming
fields. A counted port patch reconstructs the local draft struct from the native
four-argument signature. Please reconcile the upstream declaration/draft while
retaining the PS1 instruction output.

### F6 (2026-10-12). Battle text-size call spells out a MIPS hidden return pointer
Overlay 0B declares `func_8002AF70(void *, s32)` and passes `&v.unk0` as its
first argument. The resident definition returns the four-byte `TextSize` struct
from a single string argument. On native x86, the mismatched call treats the
four-byte local result as the string and writes its hidden return to the wrong
stack address; battle help setup aborts in `__stack_chk_fail`, inlined into
`func_800F4F44`. The port redirects this one call through
`port_measure_text_into`, compiled with the resident struct-return declaration.
Please document this ABI boundary rather than treating the overlay prototype as
the logical function signature.

### R1 follow-up: effect helpers
Native implementations now also cover E3EEC (effect-frame orchestration),
CF808/91C/8EC (orientation and transforms), CF9B0/CFA00 (scalar/vector GTE
interpolation), DFE4C/DFE54 (shared particle integration with two drag offsets),
and D4478/D40CC (projected textured rectangles and sprite batches). D4478 tests
existing MAC0 after RTPT and exits the whole batch on zero; it does not issue
NCLIP. DFE4C branches into DFE54's common body. Gravity updates stored velocity
without updating the IR used for the current position step. Please retain these
assembly details during naming and equivalent-C work.

### F7 (2026-10-12). Mode-8 dispatcher retains a renamed numeric entry
After battle the resident dispatcher still calls `func_801EF284`, while overlay
65 defines `battleEnd_main` at that address. This was a missing native function
stub despite a complete C entry body. A counted port patch uses the current name;
`config/xrefs.txt` pins it to overlay 65 because mode 8 explicitly loads file 101.
Please update the resident call/prototype upstream to the named entry. Reaching
mode 8 only establishes post-battle dispatch; it does not distinguish victory
from game over or establish visual correctness.

### F8 (2026-10-12). Battle positional sound accepts either a unit ID or a vector pointer
`func_800E976C` projects a unit position or an explicit `SVECTOR`, then maps screen
X around 160 to a sound-pan value around 128. `func_800C2200` passes a stack vector
when dispatching battle sound through `func_800AD90C`. The original
`arg0 & 0x90000000` discriminator identifies PS1 pointers but misses native ARM
stack addresses. The real DS opening battle halted with SystemError('S', 310):
matched savestate/DTCM return addresses trace `func_800C2200` -> `func_800AD90C` ->
`func_800E976C` -> `func_800E750C` -> `func_800E716C`'s unit-index check.
A counted port patch distinguishes small IDs (0..17, with -1 handled first) from
native pointers above the 2 MiB PS1 image. Please name/document this tagged
argument and sound-pan conversion; preserve the original PS1 address-bit test
for matching code generation. No decomp source was edited.

### Ordering-table links followed by the CPU (2026-10-12)

`docs/wip/ovl_0a_800B4A5C.c` follows `g_gameState->unk1C->unk10[buffer][8]` as a pointer before inserting the full-screen TIM sprite and texture-page packets. Original MIPS at 0x800B4DC4/0x800B4DCC and 0x800B4E40/0x800B4E48 confirms that it reads the stored link, rather than taking the address of bucket 8. This is valid in the PSX physical RAM mirror. Native 24-bit OT links must have the arena high byte restored before CPU access, exactly as in the GPU walker. The port applies a counted two-site `PSX_OT_PTR` patch; please annotate this low-RAM alias behavior when naming this routine.


### F9 (2026-10-13). KO and status controls write through absent texture-animation sets

Further low-RAM command evidence: direct disc-3 Ark status processing in
BFAF4 reaches status bit 0x400 with `Battle.curCmd == NULL`. Original MIPS at
800BFE94 loads Battle+0x1088 and 800BFE9C reads its owner at +4 without a NULL
guard. A counted port patch maps that read through PSX_NZ; both host Ark
fixtures then pass 12000 frames and reach battle-end window setup on longer
runs. Please annotate this dependency alongside F4's idle command reads.

Additional optional-motion evidence: direct disc-3 Ark startup selects Steiner
model 0x028F, whose default-motion ID is 0xFFFF. AB810 nevertheless reads the
absent motion header (`lw` at 800AB8F8) and relocates its low-RAM fields. The
port maps the local dereference, preserving the model's NULL state. Please
document this optional default motion alongside optional texture animations.

### R1 follow-up (2026-10-13). Morph interpolation/projection entry DF15C

The port now supplies DF15C, exercised by the disc-3 Meltigemini effect path.
Original assembly chooses 40-byte descriptors for a masked mesh index from
model+8 and model+5C, reads per-part vertex counts and eight-byte vertex arrays,
composes model+20's 32-byte matrices with GameData+18's camera matrix, then
interpolates the two vertex arrays through INTPL and projects in-place triples.
Projection rounds writes up to three vertices and preloads a final next triple;
these tail/register effects are retained in the port. Please name the model
and descriptor fields from this evidence rather than removing the apparent
extra loads/writes in matching code. Three hundred original-MIPS comparisons
use the production GTE; only the ignored upper padding of C4 is normalized.

Native DS first-fault capture identifies `func_800BD5A0` when Marcus dies. Marcus and Blank have no texture-animation set (`BtlUnit9C.unk50 == NULL`). Original disc MIPS at 0x800BD5E0/600/614/628 performs unconditional stores at +0x2c, +4, +0xc and +0x12. Low RAM is valid on PSX; on DS these addresses alias IRQ/raster instructions. This extends F4 beyond Baku's initialization. The port maps reviewed texture-animation dereferences into its low-RAM shadow while preserving the NULL loader state and matching decomp source. Related status, revival, script and summon accesses are inventoried together (36 dereferences across ten routines). Please annotate the optional animation-set/low-RAM behavior when naming these routines; do not insert guards into matching PSX code merely to make native execution safe.


### F10 (2026-10-14). Battle UI consumes rendered backbuffer copies

A PC software watchpoint on the ported decomp code identifies a DR_MOVE from
(528,167), 32x48, to (960,432), reached through `func_800F4A00` -> `ContinueDraw`
inside the battle VBlank callback. These coordinates belong to the selected
320-wide backbuffer; they are not a standalone texture upload. `func_800F4A00`
saves/restores the embedded drawing state through `GetDrawEnv2` and the
`D_80105160` array while resuming its UI ordering table. Please annotate this
rendered-image dependency alongside the cached-name/value routines
`func_800F2DC0`, `func_800F2ED0` and `func_800F3A8C`, and preserve the ordering and
colour/mask bits when naming the state/packets. The native GX backend previously
copied stale CPU VRAM here, producing a blank value atlas. Generated-texture
comparisons against the software renderer now match after restoring offscreen
writes, backbuffer readback and original-primitive shadow interpolation. This
is source/port-runtime evidence, not a new original-PSX breakpoint claim. No
decomp source was edited.

### F11 (2026-10-14). Embedded HUD state bypasses the cached DRAWENV

The F4A00 insertion sets E6000001 (force-mask), E1000600 plus a texture window,
and the draw offset/area from `g_drawBufs.unk0 ^ 1`, then restores embedded state
through `D_80105160`. It does not call PutDrawEnv to update a host-side cached
DRAWENV. In the native port trace, the cache names buffer (0,0) while actual GP0
E3/E4/E5 select (320,0), 320x218. A 48x4 HUD draw at (592,157) is therefore still
on screen. The GX size/origin check previously misclassified it as offscreen.
The experimental renderer now resolves these commands against separate buffer
histories. Please distinguish the cached PsyQ DRAWENV from the GPU's active
embedded area/offset when naming the HUD packet arrays and restore routine.

`o0b` C9040 advances texture animations: nonzero `unk1` selects fixed-point frame
indices with a cached previous frame; the `unk0 & 4` branch scrolls rows with two
MoveImage operations, including wraparound. With scroll frame zero, Ark repeatedly
moves (640,1), 96x190, onto itself and emits a zero-height wrap copy. The port
can omit these operations when mask settings cannot change pixels, avoiding
false texture invalidations. A blanket guard in matching PSX code is not needed.
These are decomp-source and port-runtime findings; no new original-PSX breakpoint
or decomp modification is claimed.

### F12 (2026-10-14). Effect mesh variant and module export argument types

`o0b` DD27C is C58E4's eight-list mesh packet emitter with an extra signed OT
bucket bias. Its textured faces use the model's page directly rather than
OR-ing in source flag bits 5–7. Untextured Gouraud faces emit a leading E1 draw
mode and a length-bearing NOP inside the same OT packet; their semitransparency
flag is also propagated. The culled untextured Gouraud triangle has a different
delay-slot write from C58E4. The port shares the packing core and checks both
variants against the original instructions; no matching source was changed.

Kuja encounter 891 reaches export table offset 0x2A4, E7ACC, with a raw module
pointer to a three-word scale vector. Its unit ID remains scalar. This was an
omitted data-pointer argument in the port's effect VM bridge. A broader audit of
the generated 216-function export table against PC/ARM DWARF finds 68 further
entries with omitted declared data pointers, plus printf's fixed format pointer.
The port now decodes those arguments, including stack words 9–11, while leaving
function callbacks as guest code addresses. Scalar-typed pointer parameters and
embedded struct pointers still require explicit semantic review; C prototypes
alone do not establish the entire module ABI. `tools/audit_effect_exports.py`
provides a repeatable read-only check of the declared data-pointer subset.

### F13 (2026-10-14). Gouraud frame fan and circular-line effects

D59D0 is D57D0's untextured Gouraud sibling, emitting three triangles followed
by a draw-mode packet. Its first RTPT's MAC0 value gates the entire fan without
NCLIP, and its SZ3 supplies the depth bucket for all three triangles and the
mode. The later triangles update only selected input GTE registers. D597C is
an internal linking helper using the caller's registers, not a normal C entry.
The original PSX Wave route reaches D59D0 from D2938, independently of the port
instruction comparison.

D5568 walks the circular position buffer as Gouraud lines, while D50A0/D5C30
derive ribbon widths. It nudges the previous projected endpoint by one pixel
toward the new point on each nonzero axis, fades each segment with DPCS and
links a draw-mode packet before that segment. Please preserve those distinctions
when naming these emitters and their shared context fields.

E2000's `EffTailSpawn.unk3C` callback receives two writable SVECTORs, frame and
duration, and returns a value used to stop the animation. Kuja's loaded module
supplies guest code at 0x801E8C84 here. This call now enters the effect VM in
the portified copy, retaining raw stored callback words and scalar arguments.
It is separate from the six/nine-word element and seven-word pool callbacks.


### F14 (2026-10-15). D50A0 projects joined 3D effect ribbons

Valia Pira's Holy route reaches D50A0 through D0E14/D1494. Compared with the
screen-space D5C30 ribbon, this routine computes segment/view cross products
with OP/SQR, normalizes via CFA80, applies signed ring-entry width with GPF,
and projects joined +/- edges into GT4 packets. Context +0x28 is the reference
vector, +0x30..38 saves diagonal rotation words, +0x3c/40 caches the previous
projected edges, and +0x44..58 holds current/next signed coordinates. It tests
existing MAC0 after RTPT without NCLIP. Please retain these distinctions when
naming the ribbon emitters/context fields. Original-instruction comparisons
cover full context/packet/OT/GTE state; native and original PSX Holy videos
are retained under the port's ignored build/validation directory.


### F15 (2026-10-15). Particle low-RAM vectors and module count returns

Valia Pira reaches E0168's plane modes with Pool6CEntry.unk64 == NULL. The
original PSX breakpoint at 800E0B7C shows v0=0 before its unconditional lhu at
+2; the original low-RAM height is zero. E0C88 repeats the read in another mode.
Related attraction modes read all three coordinates or forward unk64/unk68 to
CF5F8, D0674, CFCA8 and D044C. The port applies PSX_NZ at all 16/5 reviewed
read/pass sites, preserving stored NULL values. Please annotate these optional
vectors and their low-RAM dependence rather than adding matching-source guards.

Hi_SplitMdlVertex's third s32 argument is a writable vertex-buffer pointer.
It reaches DF940's SPLIT_PRIMS and is retained in BtlMesh.unk18. The effect
export at +0x304 needs pointer conversion even though its declaration is scalar.

Loaded MIPS also consumes DF738's forwarded primitive count and DD0DC's
forwarded mesh count. The wrappers are declared void in C, but original MIPS
leaves their callees' v0 results intact (DF7A0 and DD10C). Native optimization
can discard those results. This caused Valia's module to interpret a pointer
as a mesh count and walk beyond its vertex buffer. DD0B0 has the same mesh-count
wrapper pattern. The portified versions explicitly return full 32-bit counts;
please investigate matching return declarations/forwarding for all three.
Changing the decomp itself still requires PSX make verify. The export audit now
checks these return contracts and the source-reviewed scalar buffer argument;
an old ELF fails all three return checks. Both current target ELFs pass.


### F16 (2026-10-15). Ribbon, plane projection, particle integration and warp hooks

D4D6C is the fixed-X/Z-width counterpart of D50A0: ring widths scale a packed
2D direction at context +0x28, Y stays fixed, and projected joins are retained
in registers rather than written to the context. Valia's Firaga reaches it
through D0FFC/D1494. Original and native motion captures are retained.

DDF10 transforms each bone's vertices and clamps Y to model +0x5a, tagging
clipped entries with depth bit 28 before camera projection. Halfword SZ writes
preserve those flags and padded triple tails. Tantarian, Kraken and Lich use
this path. The port's original-instruction projection oracle compares raw GTE
state and both full vertex arrays.

E0014 scales particle velocities at +0xc..10 by the coefficient at +0x12, uses
GPF/GPL to integrate position, and adds the supplied bias in Z/X/Y order. New
Y acceleration is stored for the next tick; it is not included in that tick's
GPL inputs. Please preserve these details when naming the particle fields.
The port covers null bias and aliases with original-instruction comparisons.

E6764's arg13 is a per-vertex deformation callback supplied by Silver/Nova
Dragon's loaded MIPS module. It must remain a code word across export +0x248
and execute via the module bridge when called. The port's counted callback
patch fixes direct jumps to 0x801e7704. Native/original clips cover the warp;
module/native/module recursion has a focused vector mutation regression.


### F17 (2026-10-15). Absent bone arrays during enemy entry

Disc-1 battle 21 (Sealion/Black Waltz <01>) reaches C57A4 at frame 691 with a
valid BtlUnit9C but unk20 == NULL. The camera-height query indexes the missing
bone array before the model's pose matrices are ready. Original MIPS at
C5850..5C loads the base, adds boneIndex*32, and unconditionally reads +0x18.
The port maps the array base to low RAM before indexing, preserving the NULL
field. Nine indexed consumers across ovl_0b.c, ovl_0b_2.c, ovl_0b_3.c and
ovl_0b_o1.c share this array contract (height queries and bone attachments).
Please annotate the optional pose-array state when naming these paths; a NULL
check inserted into matching PSX code would change the original behavior.

### F18 (2026-10-15). BIOS string append in battle rewards

`func_80063108` starts with a BIOS A0(15h) `strcat` trampoline, not a
game-specific text operation. Player-input victory in disc-1 encounter 21
(Black Waltz/Sealion) reaches it in the reward path; idle defeat does not.
Please name this entry as the BIOS append wrapper. The remaining instructions
in its current assembly chunk belong to the following animation routine and
must not be treated as part of the string operation.

The port supplies zero-terminated byte append, preserving high-byte game text,
the original destination return value and BIOS NULL refusal. Nonzero low-RAM
arguments are mapped before access; NULL is checked before mapping.

### F19 (2026-10-15). First-use summons and Ifrit's released bone data

GameData.unk55E (u16 at +0x55e) records first-use summon bits consumed by
ovl_0b D6188 and D_800F72A0. A clear bit upgrades the short effect to the full
first-use effect. Please name this field/table: importing characters/events
from a native fixture while retaining a late-game PSX baseline's mask selected
different Ifrit animations. Both fixture launchers now set this mask explicitly.

Original PSX full Ifrit reaches DC940/DC9D0 with model 8014c090, bone 35 and
bone base 00003c30 after CD sound-bank data overwrites the released model.
The low-RAM read at 40a4 is real (BIOS kernel words, not zero). The native
query must map nonzero KUSEG before indexing, in addition to the earlier NULL
bone cases. The port patches DC9D0's three position reads and DCA18's matrix
copy, retaining the stored base word. Please document this lifetime rather
than adding source guards that alter original PSX behavior.

The port's old immediate full-read delivery advanced to a subsequent bank
overwrite while queries still ran, producing base ad8c996f and a native crash.
Sector events are now staged and reviewed blocking loader waits pump the
drive. This preserves progress without imposing a measured physical seek model.
The low-RAM BIOS contents remain a separate fidelity gap, explicitly tracked
in scenario-testing.md; successful execution does not establish matching final
particle positions.


### F20 (2026-10-15). BIOS data changes null field-animation behavior

Original BIOS boot initializes low RAM word zero to 3. The game retains it
through normal field and battle execution. AB3CC reads the absent animation
header count through NULL, then AB988/AB994/AB9A0 write the inline third-entry
flags, position and speed at low RAM +0x2c/+0x34/+0x3a. The original field probe
stops with v1 == 0 at AB988 and completes those stores. Please annotate this
behavior: the earlier port assumption that the count is zero was wrong.
The port maps all three writes without changing the optional stored pointer;
AB0B0 and BB090 must still receive NULL for their explicit guards.

The optional BIOS boot-image asset now reproduces Ifrit's F19 instruction
window at 40a4. Capture must occur after BIOS boot, not by copying ROM alone;
startup modifies 1066 bytes of the copied kernel. This verifies the queried
data and position result, not complete summon graphics parity.

### F21 (2026-10-15). KO motion checks without a current command

B8FF8, B9418 and B97A0 compare the KO unit against curCmd->unk4 with no NULL
check. Original MIPS B915C/B9514/B986C read +4 unconditionally. The expanded
spell corpus reaches B97A0 through BDD24/BFAF4 with curCmd == NULL after Sleep,
Thundaga and Comet; this also reproduces without the new BIOS image. The port
maps only the dereference, retaining the NULL and low-RAM owner word rather
than skipping the motion transition. Please preserve/document the original
access when naming these related KO helpers.


### F22 (2026-10-15). Shared guest/main character records and trance fixtures

New Game's records 5/6/7 use presets 8/10/12 and flags 0x15 (Cinna/Marcus/Blank).
Their main-character variants are presets 5/6/7 (Quina/Eiko/Amarant), with flags
from 33A50 and model indices from 33280. Please name Character.unkA's 0x10 guest
variant flag and unk34.row's preset/ability row; conflating record index with
character model produced misleading synthetic ability coverage in the port.
The 0xFE event opcode sets flags/row before 333B0's equipment/stat update.

The fixture loader now checks 33964(preset) against D_8006CAA4[id][guest], uses
33A50/33A1C/33D38/33D8C for flags and default name/equipment, then recalculates
stats. Explicit presets are optional so canonical records and guest tests keep
their existing semantics. Corrected model observations are Quina 9, Eiko 10 and
Amarant 13; the ordinary transformation paths pass for all eight main characters.

Event byte D_8007AEF0.b[0x10] enables trance in BB4A0; Character.trance at +0x20
initializes BtlUnit.unk78. Enemy damage in B81C4 saturates that gauge and calls
btl_addStatus(0x4000), which queues command 0x3b through the normal path.
Please name these gates together. A full saved gauge alone does not force a
transformation when the global flag or guest variant disables it.

### F23 (2026-10-15). PsyQ image-read transfer helper

`D_80066614 + 0x1c` points to `func_80014AA8`, used by `StoreImage` and
`StoreImage2`. Please name it as the GPU VRAM-to-CPU image transfer helper.
At 80014B58–80014B68 it computes `(width*height+1)/2` GPU words after clamping
dimensions; the CPU tail writes whole words at 80014CAC and advances the
destination by four bytes at 80014CB4. The port previously copied exactly the
requested halfwords and left odd-count padding stale. Synthetic GPUREAD
comparison with DuckStation returns zero in that high halfword; the port now
preserves the whole-word result and checks caller guards. No PSX code changes
are requested, just the helper's role and word-count semantics.

### F24 (2026-10-16). PsyQ image-upload helper and preserved mask state

`D_80066614 + 0x20` points to `func_8001486C`, the CPU-to-VRAM counterpart of
the image-read helper in F23. Please name its image-upload role. It resets the
GPU command buffer with GP1(01h), then emits GP0(A0h) and the image data; that
reset does not reset E6 mask settings. The port's `LoadImage` previously
overwrote all pixels regardless of those settings. It now respects protected
destinations and force-mask while consuming each source pixel. Controlled
GPUDUMP transfers and native ARM9 checks cover these semantics. No PSX
instruction changes are requested.

### F25 (2026-10-16). AKAO fine-pitch argument across native ABIs

`func_8005DFE0` takes `u8 fine` and returns `u16`; its three callers cast it to
`s32 (*)()` and pass signed `AkaoChannel.unk108`. On ARM AAPCS the callee relies
on the caller to extend narrow arguments, so fine=-1 incorrectly arrives as
ffffffff rather than ff. The original MIPS code explicitly masks a2 with ff at
8005E0B4. The title's channel 4 then stores detune f516 rather
than fffffff5 and programs voice 1 pitch 3ff4 rather than 0ad3. The first PCM
difference is sample 267918; instrument RAM is identical. The port removes these
three casts with a counted patch, retaining the real prototype. Please name the
fine-pitch parameter/field and document why matching PSX callers use casts; do
not change original codegen merely to accommodate the ARM ABI.

### F26 (2026-10-16). Event texture-animation opcodes use nullable tables

`evt_execInstr` opcodes C0/C1/C2 check the model pointer but unconditionally
address its `unk50` texture-animation array. Field 450 on all four discs,
disc-3 field 2300 and disc-3/4 field 2450 reach C0 with that array NULL under
synthetic New Game story flags. Loading BIOS low RAM does not remove the native
fault. Original instructions at 8003E7A0..7E0 and 8003E8A4..8E4 load the table
pointer without checking it, then index by 20 bytes and write +4/+C/+12; C2
has the corresponding flag clear at 8003E9A8 onward. The port maps all seven
accesses to its PSX low-RAM shadow, preserving the writes instead of skipping
the opcode or changing the decomp. Please name this table and document the
original nullable-table behavior while retaining PSX codegen. Disassembly and
pre-fix host backtraces are under `build/validation/field_coverage_initial`.

### F27 (2026-10-16). Animation length and script object distance read low RAM

The full field-entry sweep exposes two further native NULL reads. Disc-1 field
259 calls `func_800216D8` while its animation lookup returns NULL; the original
800216F8 executes `lhu v0,2(v0)` without checking the lookup result. Disc-2
field 1663 evaluates script distance opcode 61 with a zero object value.
Original 80052640..54 ORs in the KSEG0 base and loads +14/+1C, so the zero
value still denotes physical low RAM. The equivalent C restores the pointer
through a port hook and currently leaves zero as native NULL. The port maps
the animation halfword and the local restored angle/distance object through
`PSX_NZ`; it retains ordinary nullable resource/object lookup results for
their other callers. Please name/document these original behaviors without
adding PSX guards or changing matching codegen. Original disassembly and
pre-fix backtraces are under `build/validation/field_coverage_all`.

### F28 (2026-10-16). Movie ring framing and streamed sound boundaries

The port now has a bounded STR/MBG frame assembler, tested on all 43,379 playable
frames in the user's four US discs; see `movie-validation.md`. Please document
the original `func_8019BA70` as the video-sector ring update and
`func_8019BCE8` as the audio/metadata-sector ring update. The reader
`func_8019C2F4` returns compressed data in reverse physical sector order:
ten sectors per frame, audio indices 0/1, video indices 2..9, with 0x8F4 video
payload bytes after each 0x20-byte header. The first compressed bytes are in
physical sector 9. The original `Ovl64StreamSect` carries two 0x24-byte camera
records at +0x14, selected by `D_8019DE10.unk10`; keep both until that selector's
full semantics are named. Some ISO movie extents continue with frame-zero
padding after the frame count at audio-sector +0x60. Disc-4 MBG116's final
frame retains camera data but has AB-filled bytes +0x80..+0x7FF instead of AKAO
audio. That tail is not an audio-length field. These are format observations;
movie decode/playback and synchronization remain unimplemented in the port.

### F29 (2026-10-16). FF9 movie VLC v2 expansion matches original coefficient streams

`port/psx/movie_vlc.c` supplies a bounded C interface for the BS v2 coefficient
expansion path of `func_8019AEC0` / `func_8019B00C`. All 43,379 movie frames in
the four US discs expanded successfully; beginning/middle/end samples from all
57 movies (171 cases) match the original MIPS routine's complete MDEC command,
coefficients and FE00 padding byte-for-byte. See `movie-validation.md` for
reproduction and limits. Please document `func_8019AE44` as initialization of
the signed AC lookup from `D_8019DCFC` (108 run/level pairs), and `func_8019AE98`
as the expansion-work limit setter. The original uses GTE LZCS/LZCR for prefix
selection, not IDCT. The port's new C entry has explicit input/output bounds and
only implements version 2, which is the version observed in every scanned FF9
frame. It does not yet replace the register-ABI v3/chunk-resume entry points or
implement MDEC pixel reconstruction.

### F30 (2026-10-16). Movie MDEC environment tables and output reconstruction

The native decoder now accepts the quantization bytes uploaded from
`D_8019DEC8` (64 luma + 64 chroma) and signed scale halfwords from `D_8019DF4C`
(64 entries). These follow command words at `D_8019DEC4` (40000001h) and
`D_8019DF48` (60000000h), uploaded by the original reset path
`func_8019D020` through `func_8019D110`. Please name/document these environment
tables as MDEC inputs rather than video pixels or VLC lookup data. The two
default quant arrays are identical in the user's US overlay. The port's
integer macroblock reconstruction now agrees with DuckStation MDEC DMA output
on 3,492 synthetic/real cases; see `movie-validation.md` for the exact coverage,
ARM9 evidence and original-hardware accuracy limits. This does not replace the
original player's DMA callback, frame pacing, MBG masks or streaming sound yet.

### F31 (2026-10-17). Call before declaration of the pad read helper

After the decomp's main-exe naming merge (`f024a5f`), `src/main/main.c` calls
`func_8004E8D4()` (returns `g_padPortHeld[0] & 0x3FFFFFF`) about 2,700 lines
before its `u32` definition, with no prototype. gcc 2.8 accepts the implicit
`int` declaration; modern gcc rejects the later definition as conflicting. The
port declares it in `port/include/port_decls_gen.h`. A prototype in `ff9.h`
(or a name, since it is the masked held-buttons read) would remove the need,
if it keeps the PS1 output identical.

The same merge names the BIOS file and string calls `open`, `lseek`, `read`,
`write`, `close`, `nextfile`, `strcpy` and `strcat`. The port maps these to
`psx_bios_*` in its prelude so they don't replace the C library on ARM/x86;
no decomp change is needed for that.

### F32 (2026-10-17). Sequence the script reads in evt_execInstr case 0x10

`evt_execInstr` case 0x10 is `func_800384D0(func_80038150(), a, *g_evtCurObj->unk0++, 0);`. Both the first and
third arguments read the script stream, and C leaves their order unspecified. The PS1 code (`8003a048`) calls
`func_80038150` (the target object) and then reads the key byte. x86 GCC reads the key first: the host resolved
the wrong object in field 259 at frame 319 and never ran object 6's function 0x17. The port patches it
(`config/patches.txt`). Assigning the target to a local first, e.g.
`o = func_80038150(); func_800384D0(o, a, *g_evtCurObj->unk0++, 0);`, would say it in the source if it keeps the
PS1 output identical. The port's `tools/arg_order.py` finds no other call with two arguments that read shared state.

### F33 (2026-10-17). Data words inside ovl_0b's `.text` at 0x800CF580-0x800CF5A4
The port wants to reuse the PS1 RAM that holds an overlay's MIPS code (never executed by the port). With every
overlay's `c` segments overwritten after loading, field 259 (with its encounter), Ark, Ifrit, Kuja and Zidane's
Trance render identically to unmodified runs, except for one block inside `ovl_0b_3`'s code: 0x800CF580-0x800CF5A4.
It is data: `D_800CF580[5]` (declared in ovl_0c.h), the word `func_800CF588` that `btlbg_setBrightness` /
`btlbg_getBrightness` store and read (the battle background's colour scale), and `D_800CF5A0` (the OT shift of
`btlbg_setOtShift`). Asked: split it out of the text as data (like the `textbin` table at 0x28A80) and name it,
so the code ranges in the split config are exactly the bytes nothing reads. A list of any other data kept in
`.text` (in any overlay or main) would let the port reuse those ranges without its own exception list.

### F34 (2026-10-26). Buffers named by their PS1 address in headers
`TITLE_PACK_BUF` (ovl_14.h), `DISC_PICTURE_BUF` (ovl_3e.h), `CARD_VRAM_SAVE_BUF` (ovl_36.h), `SAVE_BUF` (ovl_39.h)
and `g_tmCardData` / `g_tmDecks` / `g_tmOpponents` (ovl_0d.h) are `((T *)0x801CDF00)`-style constants. On the PS1
that is the buffer; in the port a pointer is native, so the title's TIM pack read went to address 0x801CDF00 (the
host crashed at boot, CdGetSector from cd_onSectorReady). The port redefines them through `PSX_ADDR` in
`port/include/port_prelude.h`. Asked: name such buffers as symbols (`extern u8 D_801CDF00[]` placed by the
linker, as other fixed RAM is) or one macro the port can redefine for all, so new ones don't break a port build.
Since then (2026-10-26): `TM_IMAGE_FILE` and `TM_PRIM_BUF` (ovl_0d.h, both `0x80104A00`), used as numbers
(`(void *)TM_IMAGE_FILE`, `ctx->unk8 * TM_PRIM_BUF_SIZE + TM_PRIM_BUF`): Tetra Master's image file read went to
0x80104A00 and crashed the host on entering a card game. Redefined in the prelude too; the same ask covers them.

### F35 (2026-10-27). fldmdl_drawMesh's OT index isn't clamped (annotation)
`src_equiv/ovl_0a/model_link.c`: a quad's OT index is `(z0 + z1 + z2 + z3) >> 4` (up to 0x3FFF) and a triangle's
AVSZ3's OTZ, used as `&ot[z]` with no clamp, as in the original asm. In d2_f1750 (walking about) a polygon lands
past the field's OT; its link word (a KUSEG packet address) overwrites another ModelObj's model pointer, which the
PS1 reads through its RAM mirror. The port leaves such a packet unlinked (counted patch, `PSX_OT_OUTSIDE`). Asked:
a comment at the link saying the index can pass the table (and what lies after it), so the next reader doesn't take
it for a port bug. No behaviour change.

### F36 (2026-11-02). btl_loadEnemyModelsStep reads ids[-1] on the first model
`src/ovl_0b/btl_main.c` case 2: `dup = &ids[cnt] - 1; do { if (*dup == w) goto next; ... } while (dup >= ids);`
compares the first model id with `ids[-1]` (`ids = task->fileIds.h`), the high half of `task->dst`, before the
loop test. On the PS1 that is 0x801F (`battle->partyBufs[0]`, KSEG0), never a model id. The port's `dst` is a
native pointer: on hardware it was 0x009Bxxxx, so the Bomb (model 155 = 0x9B) counted as already listed and was
never loaded, and battle 334 (Prima Vista, field 60) crashed in btl_applyModelTextures on a NULL header. The port
compares only inside the list (counted patch). Asked: a comment at the walk that the first compare reads the word
before the list (and what lies there), or a form that doesn't if one still matches.

### F37 (2026-11-04). The battle camera script's subPhase cue (flag 0x4000) unnamed
`src/ovl_0b/btlcam_o1.c`: `if (flags & 0x4000) { arg = *pc; if (arg == 1) g_btlCamera.endIntro = arg; else
battle->subPhase++; pc += 2; }`. A scene cue: 1 ends the intro camera; any other value increments Battle.subPhase,
which the victory camera uses as "fade now" in BTL_PHASE_EXIT (the port's rewards under the battle wait for it).
It can increment subPhase in any phase. On hardware, battle 21 (Black Waltz 1 + Sealion, Zidane alone) ended in
BTL_PHASE_END with subPhase 1 = BTL_END_DEFEAT, Zidane alive: btl_checkSideDefeated keeps a DEFEAT already in
subPhase (`if (subPhase != BTL_END_DEFEAT) subPhase = VICTORY`), so a subPhase bumped to 1 during combat turns the
win into a defeat that waits forever for a KO pose. btl_initState zeroes it at battle start. Asked: a name for the
flag and its argument, a comment at Battle.subPhase that camera scripts can increment it, and which scenes or
effect cameras use it with arg != 1 (any that play in BTL_PHASE_COMBAT). Sent to the decomp session.

### F38 (2026-11-04). evt_moveObjectTo's object mode is the sign of a pointer
`src/main/event.c`: EVT_OP_MOVE_TO_OBJ calls `evt_moveObjectTo(x, y, z, (s32)po)`; the callee tells the object mode
by `flags < 0` (a KSEG0 pointer) and stops when the mover touches that object (`hit != flags`). A native pointer is
positive: the walk never ended against its target. Field 312 (leaving the Ice Cavern): Garnet walks to Zidane,
stands against him, the cutscene waits for good (3DS hardware and the host). The port treats any value past
EVT_MOVETO_NO_TURN|EVT_MOVETO_3D as an object (counted patch). Asked: the object passed separately (or a named
predicate the port can redefine), or at least a comment that the mode is the pointer's sign. Sent to the decomp session.
