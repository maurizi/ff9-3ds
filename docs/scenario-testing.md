# Direct scenario testing

Launch a field or encounter through the current build's normal engine and
asset loaders. No playthrough, emulator CPU snapshot, or old executable is
needed. This supports testing later discs as soon as their loaders work.

Find an encounter in your extracted disc:

```sh
python3 tools/scenario_catalog.py disks/us/disc3/FF9.IMG --disc 3 --find Ark
# disc 3  battle 0  Ark
```

Run a fixture on the host, retaining the game's actual failure exit code:

```sh
tools/docker.sh make -j16 pc
tools/docker.sh build/pc/ff9ds --scenario tests/scenarios/disc3_ark.cfg --frames 12000 --status 1000
tools/docker.sh python3 tools/scenario_smoke.py tests/scenarios/*.cfg
```

The batch runner writes individual logs and `report.json` under
`build/validation/scenarios/`. A missing routine, crash, timeout, or failure to
reach the requested frame limit is a failure. Passing this execution check
does not establish visual correctness or DS performance.

For native DS tests, copy the fixture to the SD filesystem and compile its
path into the test ROM:

```sh
tools/docker.sh make -j16 ds AUTORUN=1 ITCMGUARD=1 SCENARIO=sd:/ff9ds/scenarios/disc3_ark.cfg
```

The selected raw disc must be present at `sd:/ff9ds/raw/FF9 CD3.img`.
melonDS folder sync does not grow an existing auto-sized SD image reliably
when another disc is added. Use a newly created SD image with a folder
containing the selected disc, or an image sized for all desired discs.
Use one emulator at a time. Capture screenshots and short videos on DS and
the matching PSX encounter; emulator saves remain useful for inspecting a
precise failure in the same build.

### Field inventory and event texture-animation regression

`scenario_catalog.py --fields` reads directory 4's field/area IDs and sector
extents using the same layout as `ovl_14::func_800A741C`. It checks directory
bounds, duplicate IDs and resource extents. For example:

```sh
python3 tools/scenario_catalog.py disks/us/disc1/FF9.IMG --disc 1 --fields --json
tools/docker.sh build/pc/ff9ds --scenario tests/scenarios/disc1_field450_texanim.cfg \
  --data build/validation/kernel_data --frames 2000 --status 500
```

The four archives contain 222/322/372/215 entries (1,131 disc/field pairs),
covering 19/33/40/20 distinct area IDs. These inventories are resources, not
canonical story-state fixtures. A synthetic field launch can redirect to
another field or fail because its story/spawn context is inappropriate; reaching
a frame limit does not prove every field initialized, all exits, or visual
fidelity. Use event/character blobs for canonical state and review original
PSX behavior when distinguishing a port defect from an invalid fixture.

The initial one-field-per-area sweep (`field_coverage_initial`) exercised 112
synthetic entries for 2,000 host frames. It had 104 frame-limit successes and
eight crashes. Seven share event opcode C0's unguarded NULL texture-animation
table: field 450 on all discs, field 2300 on disc 3, and field 2450 on discs
3/4. Loading the BIOS low-RAM image does not remove those faults. Original MIPS
accesses the table without a NULL check, and the port now maps all seven
C0/C1/C2 accesses through `PSX_NZ`, preserving low-RAM updates instead of
skipping them (F26). All seven host reproducers now reach 2,000 frames
(`field_texture_opcodes_regression/report.json`). The separate disc-2 field-70
probe redirects to field 50, absent on that disc; it needs valid story context
and is not claimed fixed by the texture patch.

Native `field450_texanim` reaches frame 2,000 with zero first-fault markers and
no protected ITCM changes. A 12-second recording covers frames 1511–1757;
`field450_texanim_contact.png` was reviewed and shows continuing field activity.
The exact ROM/ELF, checkpoint, inspection JSON and video stay in
`build/validation`. This establishes native execution beyond the former fault,
not PSX visual parity. The emulator was closed and its prior config restored;
normal DS/PC builds pass.

The expanded `field_coverage_all` sweep selects every on-disc field using
synthetic New Game event flags and the captured BIOS image. Its manifest,
per-case logs and final report stay in `build/validation`. Further failures
included disc-1 field 259 (NULL animation-length lookup at 800216F8) and disc-2
field 1663 (NULL object in script distance opcode 0x61); see the fixes below.

### Full field-entry sweep and two additional low-RAM reads

The first full sweep finished all 1,131 disc/field entries, with 1,127 reaching
the 2,000-frame host limit. Besides the two NULL reads above, disc-2 field 70
redirects to unavailable field 50 and disc-4 field 2502 redirects to unavailable
field 2500. Both fail while trying to decode an absent resource. Those two
fixtures need canonical story context; do not suppress the decoder or count
these routes as validated. Baseline report/backtraces and original disassembly
are in `field_coverage_all` under `build/validation`.

The animation-length helper now reads a missing resource's halfword from PSX
low RAM, matching the unconditional `lhu v0,2(v0)` at 800216F8. Angle/distance
opcodes 60/61 map their locally restored zero object to low RAM, matching the
original KSEG0 restore and +14/+1C coordinate loads at 80052640..54. Ordinary
lookup return values stay nullable for other callers; the fixes do not replace
missing data with fabricated animation frames or zero distances (F27).

Repeating the entire host sweep after these changes gives 1,129 frame-limit
successes and the same two unavailable-destination failures. All cases use the
captured BIOS low-RAM image and synthetic New Game event variables. Reports
are `field_coverage_low_ram_fixed/disc1` through `disc4`, with a combined
`report.json`. This is bounded execution coverage, not a complete playthrough,
field/exit assertion, or graphics/audio comparison.

The committed reproducers are `disc1_field259_anim_length.cfg` and
`disc2_field1663_distance.cfg`. Native field 259 reaches frame 2,000, transitions
into a battle, and has zero first-fault markers and no protected ITCM changes
(`field259_anim_length_checkpoint` state/inspection JSON and matching ROM/ELF).
Native field 1663 also reaches frame 2,000 with zero exception/first-fault
markers and no protected ITCM changes (`field1663_distance_final` state and
inspection JSON, matching `field1663_distance` ROM/ELF). Its separate SD image
contains disc 2; the original emulator SD configuration was restored afterward.
Both emulators were closed before switching ROMs. These native checks prove
execution beyond the prior faults, not original PSX presentation or canonical
story progression. The normal DS/PC build passes after both patches.

For an animation at a known game frame, arm the recording before that frame
rather than estimating wall-clock delays. This works after direct scenario entry
or a scripted opening run on Linux with melonDS's fast-memory backend:

```sh
DISPLAY=:99 XAUTHORITY=/dev/null python3 tools/capture_ds_at_frame.py \
  --pid 12345 --window 0x200007 \
  --rom build/validation/test.nds --elf build/validation/test.elf \
  --frame 4800 --state-out build/validation/pre_action.ml8 \
  --seconds 60 --out build/validation/action.mp4
```

Use the actual emulator PID/window/display and its matching archived ROM/ELF.
The helper checks the process's ROM argument, native instructions and RAM layout,
then polls the live game counter. A passed threshold, mismatched build, backwards
counter or timeout aborts before capture. An optional fresh checkpoint is saved
first; its JSON reports the actual saved frame, so leave a lead before the action.
Checkpoints remain tied to their executable. The tool does not pause game logic;
review the resulting footage to verify that it includes the intended action.

Fixtures use `key=value`, with comments beginning with `#`:

| Key | Meaning |
| --- | --- |
| `kind` | Required `field` or `battle` |
| `disc` | Disc 1–4; defaults to 1 |
| `field` | Field ID; defaults to opening field 70 |
| `battle` | Required encounter ID for battles, resolved against that disc |
| `party` | Four character indices, 0–8 or -1 for empty; first slot occupied |
| `level` | Optional level 1–99, rebuilding stats and healing |
| `preset.SLOT` | Optional compatible preset/ability row 0–19; applies default flags, name and equipment, then rebuilds/heals stats |
| `trance.SLOT` | Optional saved gauge 0–255; the ordinary damage/status path still triggers transformation |
| `hp.SLOT` | Override current HP after leveling, bounded by maximum HP |
| `enemy_hp.SLOT` | Battle-only current HP 1–65535 for enemy unit ID 4+SLOT (SLOT 0–3), bounded by its initialized maximum |
| `equip.SLOT` | Five equipment item IDs, 0–255 |
| `item.ID` | Set inventory quantity 0–99, preserving other items |
| `gil` | Optional money 0–9999999; otherwise retain the initializer/baseline value |
| `story` | Set the 16-bit event story counter |
| `summons_seen` | 16-bit first-use summon mask (GameData+0x55e), defaults to New Game's 0 |
| `event.OFFSET` | Set an individual event byte, offsets 0–0x7ff |
| `characters` | Exact 9 × 0x90-byte canonical character records |
| `events` | Exact 0x800-byte canonical event variables |
| `seed` | Override initialization RNG seed; defaults to 1 |

Blob paths are interpreted relative to the process working directory; use
absolute SD paths on DS. Imported records contain no runtime pointers. Blob
values are applied first, then explicit overrides. Without blobs, the normal
New Game initializer supplies the baseline. The Ark and Meltigemini fixtures
are synthetic combat setups, not canonical late-game saves. Enemy names list
types available in a bundle; story flags can select formations or scripts.

Current limits: battle fixtures exercise isolated encounters; returning to a
fully restored parent field/world state is not yet implemented. Field story
and spawn requirements still need appropriate fixture flags. RNG seeding
does not by itself guarantee deterministic wall-clock/input behavior.
Canonical save-data extraction and scripted action/phase assertions will
extend these fixtures; loading a boss is not equivalent to covering all of
its attacks, victory, defeat, or cutscene transitions.

Enemy HP overrides run once when entrance animations finish and the loader
enters combat, before the entry event script. They change current HP only;
maximum HP, status and scripts remain intact. Requested absent units or HP
above their initialized maximum fail explicitly. Boss scripts may subsequently
replace HP or use HP thresholds for scripted exits, so an override does not
establish coverage of a phase or a normal victory by itself. Gizamaluke with
synthetic story=10000 has maximum HP 65535 at this point and exits without a
player command when set to 1 HP; it is unsuitable as an ordinary kill test.

`tests/scenarios/disc1_black_waltz_rewards.cfg` instead reaches ordinary attack
commands against both enemies, victory/rewards, then field mode on the host.
To reproduce the input route, run this inside the build container:

```python
import subprocess
command = ['build/pc/ff9ds.elf', '--scenario',
           'tests/scenarios/disc1_black_waltz_rewards.cfg',
           '--frames', '65000', '--status', '3000']
for frame in range(1000, 30000, 40):
    command += ['--press', f'CROSS@{frame}+4']
subprocess.run(command, check=True)
```

Host validation retained under `build/validation/enemy_hp_report.json` checks
one-time application to both enemies (1/472 and 1/229), return to field,
and rejection of absent slots, excessive HP and zero HP. The original attack
trace is `black_waltz_low_hp.log`. This does not prove native DS or original
PSX reward-screen fidelity.

Subsequent native validation of the Black Waltz reward route retains matching
`ff9ds_black_waltz_rewards` ROM/ELF, a pre-reward state at game frame 3175 and
a returned-field state at 4633. Both states have zero first-fault markers and
no protected ITCM changes. `ds_black_waltz_rewards_action.mp4` records 60
wall-clock seconds through the victory pose, item/gil page and field return.
`psx_black_waltz_rewards_action.mp4` records 12 emulated seconds from original
victory handling through rewards and field return. Sampled motion frames were
reviewed on both platforms; reward windows/layout and exit behavior agree.
Reading final GameData.gil gives 839 on both platforms. RNG phases and input
timings differ (including item ordering); this is not pixel/timing equivalence
or audio validation. All artifacts remain under ignored build/validation.

An additional six-case host input probe (`reward_sweep/report.json`) reaches
65000 frames without process failure for Antlion, Ark, Garland, Meltigemini,
Taharka and Deathguise. All except Antlion reach field mode; Antlion reaches
the title and is not counted as reward coverage. These synthetic
fixtures lower enemy slot 0 only and use return field 350 on discs 3/4.
Attack/phase assertions and native/PSX comparisons remain due for this sweep.

Verified: opening field 70 entered directly on the host; disc-3 Ark entered
directly on both host and melonDS, revealing optional null default-motion
handling and missing battle effect emitters without playing the opening.
The two Ark emitters D4A30 and D45FC now have native equivalents that pass
original-MIPS packet comparisons. Ark subsequently reaches an unresolved
CD-loaded MIPS effect module: its header pointer at PSX 801E7700 is 801E9058,
initializer is zero, and first effect callback is 801E8178. The generated
resident overlay relocations do not cover this module. The 12000-frame batch
therefore reports Ark as a crash, and Meltigemini as missing D3ED4; neither
boss is claimed to pass. Reports live in
`build/validation/scenario_emitters_batch/`. The matched native test ROM/ELF
is `ff9ds_scenario_ark_emitters`, with a fresh-boot capture in
`build/validation/ds_direct_disc3_ark_fresh_boot.mp4`.

The subsequent `effect_vm.c` bridge executes integer/GTE glue from these
CD-loaded effect modules and calls their native battle-engine exports. The
reviewed pointer-argument masks preserve scalar flags and optional NULLs;
D044C's second argument is an output pointer carried as `s32`. Resident
callbacks keep their native entry points. The bridge passes 300 differential
integer/branch-delay/GTE-model cases, 60 cases of Ark's original initialization
code, and the native 16-word argument test under ASan/UBSan. It does not yet
claim complete MIPS-I instruction/load-delay coverage or support for every
module/export signature. Ark now reaches missing D5C30 on the host; the
original dynamic-header crash is passed, but its full attack remains unported.
Fresh ARM validation reproduces that later missing D5C30 stop at frame 1595
(`ds_ark_effect_vm_probe.ml8`, matching `ff9ds_ark_effect_vm` ROM/ELF), with
the first-fault marker zero and no instruction-region corruption. The existing
60000-frame opening-to-briefing host route also passes with this bridge.

## Subsequent Ark effect coverage

D5C30's circular-chain ribbon emitter and the shared DFF5C/DFF64 particle
integrator now have native equivalents. The differential suite compares
context bytes, packets, OT links and raw production GTE state against original
MIPS in 400 randomized cases under ASan/UBSan. D3ED4's textured effect quads
also pass the expanded 400-case quad/sprite suite (shared GTE model).

Further Ark failures exposed two portability details: the export interface's
model-pool initialization arguments are pointers into the loaded module; and
E1534 reuses effect descriptors whose pointer fields carry the PSX sign bit
after relocation. Native pointers lack that bit, so repeated initialization
added the base twice. Reviewed export masks now map model-pool arguments, and
counted descriptor patches only relocate values still within the offset range.

Fresh DS execution reaches frame 3051, executes attacks and reduces party HP,
then stops at missing D4228. The matching `ff9ds_ark_ribbons_particles` ROM/ELF
and `ds_ark_ribbons_particles_combat.ml8` retain the failure. Its first-fault
marker is zero and the ITCM instruction region matches the ELF. Videos:
`ds_ark_ribbons_particles_boot.mp4` and
`ds_ark_ribbons_particles_combat.mp4`. This does not prove the entire boss,
exit paths, visual fidelity, or performance. The host batch still reports Ark
and its low-HP variant as missing D4228, and Meltigemini as a later segmentation
fault (`scenarios_after_ribbons/report.json`).

## Effect-module ABI audit

Effect-module ABI checks can now be run independently of playthroughs:

```sh
python3 tools/audit_effect_exports.py > build/validation/effect_export_audit.json
python3 tools/audit_effect_exports.py --elf build/ds/ff9ds.elf > build/validation/effect_export_audit_ds.json
```

Build matching PC/DS targets first. The tool reads the generated 216-function
export table and compares the bridge masks with native DWARF argument types.
It returns nonzero for missing data-pointer bits or unresolved signatures.
Callback pointers are reported separately and remain raw guest code addresses.
Six optimized scalar wrappers are checked against their reviewed source
declarations; printf's fixed format parameter comes from stdio.h. This checks
declared data pointers, not scalar-typed pointers, nested structures, variadic
arguments or complete callback behavior. Keep the JSON reports for review.

## Matching original PSX encounters

The original PSX also enters Ark directly from a disc-3 field baseline using
the host fixture's pointer-free character/event records and ordinary battle
resources. `tools/psx_battle_fixture.py` reads encounter/background sectors
from the original disc, unwinds the original field before importing synthetic
story data, and stops at the resident dispatcher before the battle loader.
It changes runtime test data, preserving executable bytes.

Start Redux with disc 3 paused, then run:

```sh
python3 tools/psx_battle_fixture.py tests/scenarios/disc3_ark.cfg \
  --characters build/validation/ark_fixture_characters.bin \
  --events build/validation/ark_fixture_events.bin \
  --base-state disc3_scenario_field_ready
```

Export these exact-size blobs after `port_scenario_bootstrap` returns in the
host build: `GameData.characters` (9 x 0x90) and `D_8007AEF0` (0x800). The
referenced local baseline was obtained from the existing disc-3 Pandemonium
memory-card save, without a playthrough. A cold Redux BIOS must boot before
restoring it; otherwise this instance restores RAM/VRAM but remains in a BIOS
loop. The tool boots a cold BIOS before its own baseline restore. Do not start
Redux with `--state` before booting for this path. A pause in a currently live,
initialized field also works without `--base-state`.

The tool is runtime-validated after warm boot (`psx_fixture_warm_boot.log`,
`psx_fixture_warm_boot.png`) and from a cold paused start through automatic
boot (`psx_fixture_auto_boot.log`, `psx_fixture_auto_boot.png`), independently
of its offline range check. The
original entrance/combat video is `psx_direct_ark_entrance_combat.mp4`; the
Photon HUD screenshot is `psx_direct_ark_hud_reference.png`. Party/story data
match; input timing and RNG phases are not synchronized, and inventory is not
imported by this PSX helper.

When `enemy_hp.SLOT` is present, the helper also runs the ordinary battle
loader to original PC 800AF3A0: AF2E0 has just selected combat state 4 and
has not invoked the entry event script. It validates active enemy IDs and
their maximum HP before writing only their current HP halfwords. It then
leaves the original paused at that point. Fixtures without enemy HP retain
the earlier stop before battle loading. Later scripts can replace HP on both
platforms; this hook does not bypass them.
The Black Waltz fixture was run through this automatic reference hook:
`build/validation/psx_black_waltz_fixture_auto_hp.log` records 1/472 and 1/229,
and the reference remains paused at 800AF3A0. This verifies the live loader
path independently of `--dry-run`.

The reference helper also sets `summons_seen` (default 0) after field cleanup,
matching the host's New Game state. A late-game field baseline otherwise
retains its first-use flags and silently selects a different summon animation.
Setting 65535 removes first-use forcing; normal summon RNG still applies.

## Commands and first-use summon coverage (2026-10-15)

Use a command probe to exercise an ability's ordinary queue and effect loader
without navigating its menu for every case:

```sh
tools/docker.sh python3 tools/scenario_command_probe.py \
  tests/scenarios/disc3_ifrit_first.cfg --command 16 --ability 51 \
  --out build/validation/ifrit_command_probe
```

The tool generates a GDB script, replaces the first player Attack's command,
ability and target words, and retains the real actor, ATB and command slot.
It requires an observed player effect call, a zero inferior exit and the
requested frame limit. A signal, missing effect or timeout fails, even if the
debugger itself exits successfully. It does not prove learned abilities,
legal menu availability, animation completion or graphical/audio equivalence.
`--actor ID` restricts queue replacement and effect observations to that
character (0–8), allowing multiple party members without selecting the wrong
caster. Without it, the first player Attack remains the probe seam.
Targets are a unit mask (default 0x10); use the appropriate actor/command and
target type for each ability. The synthetic Ark arena disables magic in its
normal menu; dispatch probes there deliberately bypass selection restrictions.

The first-use Ifrit fixture reproduces a model/bank-buffer lifetime crash.
The repeated-use fixture passes with seed 1's short animation. On PSX, matching
the first-use mask reaches the full model; leaving the baseline mask 0x5686
instead gives the short animation and cannot validate that failure.
The PSX full-animation video and callback/CD trace are
`psx_ifrit_first_summon_action.mp4` and `psx_ifrit_first_summon_trace.json`.
The conditional reference probe `psx_ifrit_low_ram_probe.json` confirms a
bone-35 query through base 0x3c30 after the model header is overwritten.

Native low-RAM mapping alone passed that first access but then crashed on
sample bytes interpreted as base 0xad8c996f at frame 3095. Single-sector CD
events preserve asynchronous ordering; the first-use command probe then
reaches 12000 frames (`ifrit_command_sector_step_current/report.json`).
The initial staged-drive Beatrix test hung in main's blocking field-loader
wait; both waits now service the drive explicitly. Keep the earlier failed
logs as regression evidence. The final broad sweep uses an archived executable
to avoid mixing builds (`boss_sweep_sector_wait_verified/`): all 30 cases
reach 65000 frames with exit code zero. Many end through defeat/title;
this establishes execution coverage, not all attacks or rewards.

Seventeen spell/summon dispatch probes now observe their player effects and
reach 12000 frames (`spell_sector_step_sweep.log`). The generated catalogue,
fixtures and reports remain under build/validation. This includes Fire,
Firaga, Blizzaga, Thundaga, Comet, Meteor, Flare, Doomsday, Shiva, Ifrit,
Leviathan, Bahamut, Ark, Fenrir, Carbuncle, Phoenix and Madeen; it is not all
commands, alternate summon forms, targets or phases.

An ordinary menu route on the host also selects Ifrit (command 16, ability 51)
against Meltigemini, observes effect 445 and reaches 12000 frames
(`ifrit_real_menu_meltigemini.log`). Its generated canonical character blob
sets each learned AP slot to that character's actual mastery requirement.
The earlier Ark menu attempt correctly offered disabled magic and queued no
spell; it is not a successful cast test.

Native DS verification uses the archived `ff9ds_ifrit_sector_events` ROM/ELF
and the same ordinary menu sequence against Meltigemini. The 60-second clip
`ds_ifrit_sector_events_action.mp4` contains the full model animation, fire
blast and return to combat. Its frame-4119 checkpoint has zero first-fault
markers and no protected ITCM changes. Sampled motion frames were reviewed
against the PSX full-summon capture. The reference uses Ark as its target and
forced command words; the DS uses Meltigemini and menu input. This supports
model/effect progression comparison, not synchronized pixels or damage values.

The optional `psx_low_ram.bin` runtime asset supplies the original BIOS low-RAM
contents. `tools/capture_psx_kernel.py` captures the first 64 KiB at the original
executable entry after a cold BIOS boot. It verifies the stopped PC and executable
bytes before capture. A raw ROM copy is insufficient: the boot process modifies
1066 bytes of the copied kernel region with the supplied SCPH1001 BIOS.
The port checks the PXR1 header, exact length and FNV-1a checksum before use;
the bytes remain guest data and are never executed as native code.

Start the original emulator cold and paused with `--no-fastboot`, then run:

```sh
python3 tools/capture_psx_kernel.py --out build/validation/psx_low_ram.bin
```

Install this asset as `psx_low_ram.bin` in the runtime data directory (beside
`raw/` on host, or inside `ff9ds/` on the DS SD card). Both scenario runners
accept `--data` for isolated roots. Without the asset the port logs that it
retains zero-filled low RAM; this fallback does not reproduce BIOS-derived
reads. No BIOS/game bytes are committed.

`psx_runtime_low_ram_sample.json` confirms the original game retains count 3
at address zero and the same instruction window at 0x40a4 during Ifrit.
`ifrit_kernel_bone_output.log` verifies the native bone-35 query reads those
exact twelve bytes and produces the expected three halfwords. This resolves
that specific data mismatch; it does not establish full effect pixel parity.

The initialized image also exposed a previously hidden field-return write:
`psx_field_null_animation.json` stops the original at AB988 with the animation
pointer zero, then verifies the inline third-entry stores through AB9A0. The
port now maps the writes as well as the count read. The 64-command corpus
also exposed KO motion checks reading a null current command in B97A0 (Sleep,
Thundaga and Comet runs). All three equivalent KO helpers now map that read,
preserving the stored NULL and original motion-selection behavior.

The archived `ff9ds_kernel_ko_field.elf` completes all 30 boss fixtures through
65000 frames (`boss_sweep_kernel_ko_field/report.json`) and all 64 white/black
magic and summon probes through 12000 (`kernel_commands_ko_field/report.json`).
These are execution checks with synthetic single-actor parties, not proof of
menu legality, all battle phases, effect pixels or audio. Thundaga also passes
without the BIOS image, where the older binary reproduced the same KO crash.
The tracked regression fixture is `tests/scenarios/disc3_vivi_ko_motion.cfg`;
run it with command 22 and ability 28, 35 or 42, for example:

```sh
tools/docker.sh python3 tools/scenario_command_probe.py \
  tests/scenarios/disc3_vivi_ko_motion.cfg --command 22 --ability 35 \
  --data build/validation/kernel_data --out build/validation/vivi_ko_regression
```

A subsequent 60-case dispatch sweep covers Blue Magic (77–100), Zidane's
normal skills (101–108), Freya's Dragon skills (117–124), Amarant's Flair
(125–132), and Steiner's Sword Art (141–152). There are no signals or fatal
errors. 59 observe their effects and finish 12000 frames directly
(`extended_commands/report.json`); Spare Change is rejected for insufficient
money in the New Game baseline. The funded, multi-actor fixture
`tests/scenarios/disc3_spare_change.cfg` passes with `--actor 7 --command 28
--ability 126` (`spare_change_actor_gil/report.json`). A separate debugger
probe records gil changing from 100000 to 90001, matching the computed 9999
cost. This avoids treating a correctly rejected move as effect coverage.
The optional `gil` fixture value is also supported by the original PSX helper.
`gil_boundary_checks_verified.log` checks the unchanged default 500, zero,
the maximum 9999999, and rejection of negative/overflow values. An absent
actor produces no replaced command/effect and fails the probe, while the
unfiltered Vivi/Thundaga regression still passes. The full opening input
route also reaches 60000 frames with the BIOS asset and returns from battle
to field mode (`opening_with_boot_kernel.log`).

Native validation uses the archived `ff9ds_ifrit_kernel_ko_field` ROM/ELF.
`ds_ifrit_kernel_ko_field_runtime.json` confirms the BIOS instruction window
is loaded on DS; the 60-second `ds_ifrit_kernel_ko_field_action.mp4` shows
Ifrit's model, fire blast and return to combat. Six motion samples were
reviewed against `psx_ifrit_first_summon_action.mp4`. As above, the enemy and
timing differ, so this is not a synchronized pixel comparison. The checkpoint
at frame 4148 has no exception/first-fault markers or protected ITCM changes
(`ds_ifrit_kernel_ko_field_after_inspection.json`). The captured asset is
installed in the local normal and scenario SD roots; both emulators were
closed after validation and the normal melonDS SD configuration restored.

A separate disc-1 SD image validates Beatrix's previously failing field return
on the same archived native code (`ff9ds_beatrix_kernel_return` ROM/ELF).
The 30-second `ds_beatrix_kernel_return.mp4` shows the dark opening field and
its dialogue after the battle. Its frame-9518 checkpoint has no exception or
first-fault marker and no protected ITCM changes
(`ds_beatrix_kernel_after_return_inspection.json`). This establishes native
execution of the corrected field path, not pixel parity for every field.

## Character presets, trance and Sword Magic

New Game initializes records 5–7 as Cinna, Marcus and Blank (presets 8/10/12,
models 14/15/16, flags 0x15), not Quina, Eiko and Amarant. Selecting those
record indices alone therefore tests abilities on guest models. This limits
the earlier 64- and 60-case reports above: they established ability dispatch
and execution, not the intended main-character models for those records.
`inspect_roster.log` records the original baseline.

Use explicit `preset.SLOT` overrides for synthetic main-character parties:
0–7 are the corresponding main character presets. Record 8 is Beatrix;
other guest/alternate presets remain available only for their compatible
record. The loader validates compatibility against the game's own tables,
uses the game's default flags/name/equipment helpers and recalculates stats.
Explicit equipment, level, gauge and HP overrides follow that initialization.
Without a preset override, imported/New Game records remain unchanged.
The PSX helper requires exported canonical records with matching presets and
guest flags; it rejects a mismatched guest blob instead of silently using it.

`trance.SLOT=255` initializes the saved gauge, not a live status bit.
The synthetic trance fixture also sets `event.0x10=1`, the game's global
trance enable flag; subsequent enemy damage calls the normal status and
transformation-command paths. `--require-trance` waits for an Attack from an
actor with status 0x4000. Success also requires observing that actor's ordinary
trance-entry effect, the requested ability effect and the frame limit.
A disabled global trance flag correctly gives no transition/replaced command
(`trance_disabled_rejection/report.json`). Gauge bounds, invalid slot/preset
values and incompatible characters are checked in `preset_trance_validation.log`.

The corrected preset sweeps use the archived `ff9ds_preset_trance.elf`:

- `magic_summon_presets/report.json`: all 64 probes pass with intended presets.
- `extended_commands_presets/report.json`: 59/60 pass; Spare Change is correctly
  rejected at New Game's 500 gil. The funded tracked fixture passes on actual
  Amarant (`spare_change_real_amarant/report.json`).
- `trance_sweep_presets/report.json`: all 22 pass (eight Dyne moves, eight Elan
  moves, and trance attacks for the other six main characters).
- `sword_magic_baseline/report.json`: all eleven Sword Magic abilities pass
  with Steiner and Vivi together. `tests/scenarios/disc3_sword_magic.cfg`
  retains the explicit party/presets; select actor 3, command 31, ability 50
  for Fire Sword (other IDs: 52/54/56/57/59/61/63/65/75/76).

Targeted reports additionally record the actual model/preset/flags selected:
Quina 9/5/5, Eiko 10/6/6 and Amarant 13/7/5. Future probes retain this metadata
beside command/effect evidence so guest substitutions are visible. These
remain dispatch checks rather than proof of learned abilities or menu legality.

Native validation uses `ff9ds_zidane_trance` ROM/ELF, the tracked Zidane fixture
and no scripted battle inputs. The 60-second `ds_zidane_trance_action.mp4`
shows the transformation flash/rays, the trance model and subsequent combat.
The frame-3759 checkpoint has zero exception/first-fault markers and no
protected ITCM changes (`ds_zidane_trance_after_inspection.json`).
The same party/gauge/events were imported through the original PSX field
cleanup and battle loader. `psx_zidane_trance_entry.json` records the original
entry effect 257 and status 0x4004; `psx_zidane_trance_action.mp4` records its
next 20 emulated seconds. Six sampled motion frames were compared with the
DS clip. Enemy action/RNG timing differs, so this checks transformation and
model progression, not synchronized effect pixels or combat values.
Both emulators were closed and normal melonDS configuration restored.

## Renderer colour validation

[Controlled PSX colour checks](render-colour-validation.md) now compare the
production rasterizer with DuckStation's software GPU. All 326 synthetic colour
cases match after dithering support (253 matched before). Native trance video
and a fault-free endpoint checkpoint exercise the updated code on DS. This
resolves a specific colour-pipeline gap; rectangular character artifacts,
exact edges, full animation pixels and audio remain separate work.

## Cached HUD text and subsequent effect interfaces

The earlier screenshot-based diagnosis of stale/overlapping DS name text was
incorrect. Replaying the saved PSX VRAM through the scaler reproduces the active
DS bitmap exactly, including the name row (`ds_ark_scaler_replay.json`: zero
different pixels). Cached battle names and HP/MP sprites bypassed the font
coverage matcher and received nearest sampling instead. The matcher now includes
the reviewed atlas bounds for cached names, cached values, and direct digits.
Its positive/negative UV tests pass under ASan/UBSan. A fresh DS state at frame
2483 records 64 coverage rectangles in each frame list, including four names
and eight cached values, with no first-fault marker or ITCM corruption.
`ark_hud_same_frame_comparison.png` compares old/new filtering on identical
source pixels; `ds_ark_hud_coverage_boot.mp4` and
`ds_ark_hud_coverage_combat.mp4` retain live animation. This establishes coverage
of these sprites, not complete visual fidelity or acceptable performance.

D4228/D437C now emit native projected sprites/points and pass 600 original-MIPS
CPU/packet cases using a shared GTE model. Subsequent failures exposed raw PSX
positions and output buffers inside loaded modules, export-table entries beyond
the former 160-entry scan, and six/nine-argument callbacks. The bridge scans
216 function entries (data follows), decodes reviewed pointer arguments while
preserving scalar bit patterns, and supports up to 16 o32 callback arguments.
Local descriptor reads and D1494 buffer accesses decode pointers without changing
the module's stored pointer representation. Tests exercise late exports,
six/nine incoming arguments, zero-padding of native arguments, and 16-word
outgoing calls. Fresh native DS execution with these changes reaches frame 4251
and stops at missing D57D0, with no first-fault marker or changes to the first
16 KiB of ITCM (`ds_ark_trail_writer_stop.ml8` and inspection JSON, matching
`ff9ds_ark_trail_writer` ROM/ELF). Boot and combat videos with the same prefix
retain animation. This validates the prior pointer/callback fixes along this
route, not all module interfaces.

CF5A4 now writes circular trail positions/widths; 400 randomized cases compare
the entire buffer and return value with original MIPS, including wraparound,
byte counter rollover, and raw/native input pointers. The host Ark route now
passes the prior failures and stops at missing D57D0
(`scenarios_trail_writer/report.json`). The encounter and later content remain
incomplete; neither this progress nor a passing build proves the boss playable.

D57D0 now emits the single textured effect rectangle, preserving UV arithmetic,
context scratch writes, the existing MAC0 cull decision, depth bucket limits,
OT links and optional color fade. Its original-MIPS differential cases expand
the quad suite to 700 cases using a shared GTE model. Both targets build. The
host proceeds past frame 6000 before a raw output pointer to E750C causes a
segmentation fault (`pc_ark_d57d0_fault.log`); D57D0's native DS rendering still
requires validation.

Reviewed unit-position exports now decode position/rotation/vector buffers at
table offsets 1F8, 1FC, 200, 204, 20C, 214, 2A0, 2A8 and 2B8. Model draw
exports at 60, 64, 28C and 31C also decode their first three arguments: the
third is declared `s32` in wrappers but forwarded as a scale vector to DCA68.
Summon bone output buffers at 254/290 receive the same treatment. Fifteen
synthetic export cases verify mapping, native writes and unchanged scalar IDs.
The existing 360-case effect VM suite and both target builds pass. Both Ark
fixtures advance to a different failure: BFAF4 dereferences a null `curCmd`
while processing status bit 0x400 (`pc_ark_positions_models_fault.log`).
This interface batch still requires fresh native DS validation.

BFAF4's status-bit-0x400 command-owner read now uses the PSX low-RAM shadow
when `curCmd` is NULL. Original MIPS at BFE94 loads Battle+1088 and BFE9C reads
word +4 without a NULL guard. The matching source is unchanged. Both target
builds and low-RAM tests pass; normal and low-HP Ark fixtures each complete
12000 host frames. At 65000-frame limits both stop earlier in battle-end window
setup at missing 63118; Meltigemini now stops at missing D4768
(`scenarios_status_curcmd_extended/report.json`).

Fresh native execution of D57D0 and the unit/model export fixes also reaches
missing 63118 (`ds_ark_positions_models_current.png` and failure video).
Savestate hotkeys failed twice in that emulator instance, so no saved frame
count, fault marker or ITCM comparison is available for this run. The file
named `ds_ark_positions_models_combat.mp4` was captured after failure and is
not evidence of combat animation. The earlier trail-writer videos remain the
last verified live combat recordings. These encounters, post-battle UI,
graphics and performance still need further work.

The native 63118 entry now sets a window's tween/delay/frame and optionally
rewinds coordinates by signed animation deltas. It is the non-BIOS entry
inside the original 63108 assembly body. Four hundred cases compare the
complete window record against original MIPS, with a shared signed-delta sum,
including NULL animations/delta arrays, negative counts, byte-masked flags,
and coordinate wraparound. Both targets build. Ark's longer host routes now
reach `gameover_main`, which submits ordering tables and packets from two
stack-local `DispBuf` structures; DrawOTagEnv's ordinary arena-only link
resolution rejects those native stack links (`pc_window_setup_ot_fault.log`).

Game Over now submits its two stack-local display buffers through the bounded
native OT walker, after installing the draw environment. Both target builds and
the existing native-stack/arena/alias ordering-table regressions pass. Normal
and low-HP Ark fixtures each complete 65000 host frames, remaining in mode 8's
Game Over wait loop (`scenarios_gameover_stack_ot/report.json`). This verifies
execution through defeat, not victory rewards or dismissal back to the title.

Original PSX defeat was reached through ordinary combat after lowering only
the living party units' HP to 1 (`psx_ark_defeat_hp_override.json`); executable
bytes, statuses and enemy data were preserved. The screenshot is
`psx_ark_gameover_reference.png`,
and original state `disc3_ark_gameover_fixture`. The host's rendered Game Over
frame at 15017 matches every pixel at native RGB15 precision. PNG channels
differ by at most 7 because PSX expands the low channel bits while the host
shifts by three (`gameover_psx_host_comparison.json`). This comparison covers
the static Game Over composition, not all combat rendering.
The file named `psx_ark_defeat_transition.mp4` was captured too late: it shows
static Game Over with the emulator's debug UI, so it is not evidence of the
defeat entrance animation. The subsequent GPU-only dismissal video below is
the original PSX animation reference.

Fresh DS execution with 63118/native Game Over buffers has reached frame 9751,
including KO and subsequent Ark attacks, with no first-fault marker or changes
to the first 16 KiB of ITCM (`ds_ark_window_gameover_posteffects.ml8`, matching
`ff9ds_ark_window_gameover` ROM/ELF). The combat video is
`ds_ark_window_gameover_combat.mp4`; PSX combat counterpart
`psx_ark_window_validation_combat.mp4` has different input/RNG timing.
Native Game Over remains pending. A level-99 host attack/input probe lost,
dismissed Game Over, and survived at the title through 65000 frames
(`pc_ark_attack_probe_level99.log`); it does not validate victory/rewards.

D4768's textured strip emitter now reuses the position table per strip,
preserves alternating scale rows, the conditional UV row mask, context writes,
MAC0 early exit, and OT bucket limits. The strip suite covers both textured
and Gouraud variants in 400 original-MIPS cases with a shared GTE model. Both
targets build. Ark still passes 65000 host frames; Meltigemini advances to
missing DF15C after frame 4000 (`scenarios_textured_strip/report.json`). The
textured-strip addition requires native DS/PSX validation.

## Native Ark defeat and dismissal validation

The fresh DS run reaches Game Over at frame 14419 and returns to title/attract
mode after confirmation, saved at frame 19591. Both states have a zero first
fault marker and no changed bytes in the first 16 KiB of ITCM
(`ds_ark_window_gameover_reached.ml8`, `ds_ark_gameover_dismissed.ml8`, their
inspection JSON, matching `ff9ds_ark_window_gameover` ROM/ELF). Extracted DS
source VRAM matches every original PSX RGB15 pixel; its active bitmap matches
the production scaler replay exactly, with zero coverage rectangles in this
static TIM scene (`ds_gameover_psx_scaler_comparison.json`). This rules out a
source-image/presentation mismatch for this screen.

`ds_ark_defeat_transition.mp4` records Annihilated-to-Game-Over on the DS.
`ds_ark_gameover_dismissal.mp4` and the original GPU-only
`psx_ark_gameover_dismissal.mp4` show the fade to black and return to the title.
The contact sheets were visually reviewed. DS footage uses accelerated wall
time; PSX GPU capture uses emulated time, so these clips do not establish
timing parity. Original PSX has existing memory-card saves; the DS fixture
does not share that memory-card context, so title menu availability/preselection
is not compared here. Both emulators are closed and the normal DS SD config
is restored after validation.

The DS Ark interval from frames 3083 to 9751 takes 65012 emulated VBlanks:
about 6.15 game frames/s at nominal 60 VBlanks/s
(`ds_ark_window_combat_rate.json`). This includes pacing and rendering, not
only raster work, and remains too slow. Victory/rewards, later content,
remaining renderers and overall visual/performance correctness are incomplete.

DF15C now interpolates morph-model vertices, composes each bone with the camera
matrix, and projects the original in-place triples. The final speculative GTE
vector loads and rounded-up vertex-tail writes are preserved. Three hundred
randomized cases compare both vertex arrays and GTE state against original
MIPS using the production GTE model. Only C4's upper padding bits are normalized:
original assembly loads uninitialized matrix-stack padding there, while commands
ignore it and CFC2 sign-extends the low 16-bit matrix element. This is not a
hardware GTE verification. Both target builds pass. Ark and Meltigemini now
complete 65000 host frames (`scenarios_morph_projection/report.json`); DF15C still
requires native DS and original PSX morph-animation validation.

## Optional native timing diagnostics

Build with `PROFILE=1` to use DS timers 2/3 as a free-running bus/64 clock.
Ordinary builds neither configure these timers nor insert timing calls. A
published `port_profile_stats` block covers 128 presentation intervals within
one game mode. Decode a fresh matching state with:

```sh
python3 tools/inspect_ds_profile.py --elf MATCHING.elf CAPTURE.ml8
```

The diagnostic measures GP0 processing, GTE commands, effect callbacks,
display scaling and VBlank waiting. Categories overlap (native effect exports
can call GTE), so percentages must not be summed. It also adds measurement
overhead and is not a substitute for an uninstrumented rate check.

New builds additionally publish `port_profile_detail_stats` for GX texture
material lookup/conversion, GPU pixel uploads, palette uploads, the C58E4 battle
mesh packet emitter and ordering-table traversal. The decoder includes these
only when their publication frame matches the main 128-frame window. Older
states remain supported. Texture uploads are included in material time, and
ordering-table time includes decoded draws and their renderer work; it is not
the cost of traversing links alone. Software builds report zero GX material and
upload time. All instrumentation is disabled without `PROFILE=1`.

`GXLAZY=1 GXSHADOW=1 GXSHARE=1 SOFTDISPLAY=0` enables the experimental
demand-driven framebuffer shadow. Two PSX backbuffers retain decoded primitives;
8x8 tile progress prevents double blending. Reads replay the requested pixels,
and texture/CLUT writes finish and retire affected draws before their source
changes. Full queue/destination replacement paths preserve the other pixels.
The mode also resolves embedded HUD clipping commands to their actual backbuffer
when the cached DRAWENV still names the other one. It requires geometry shadows
and uses roughly 2.5 MiB of static framebuffer/command storage. It does not solve
GX allocation, vertex-budget or blending limitations; software remains default.

For a checkpoint that explicitly tests complete framebuffer readback, add
`CAPTURE_FRAME=1663 CAPTURE_READBACK=1`. This runs `psx_vram_read` across VRAM
after publishing the timing window and before entering the capture halt loop.
Use a fresh matching state after the capture message appears. Without this
option, a deferred framebuffer's unqueried pixels legitimately remain pending
in its shadow; its raw VRAM bytes are not a complete framebuffer comparison.
Do not derive FPS from a checkpoint's ever-increasing VBlank counter after halt.

The first fresh Ark sample ends at frame 1377: 142.08 ms/interval, including
91.41 ms GPU (64.3%), 11.65 ms GTE (8.2%), 16.12 ms scaling (11.3%),
10.15 ms VBlank waiting (7.1%), and no effect callbacks in that window
(`ds_ark_timing_probe_first.json`, matching state and `ff9ds_ark_timing_probe`
ROM/ELF). Rasterization is the largest measured cost; primitive-level timing
and later attack windows still need investigation.

Primitive-level profiling publishes a matching `port_profile_draw_stats` block
for polygons, rectangles and lines. At the 128-frame window ending 1633, the
baseline records 67.79 ms polygons and 23.55 ms rectangles within a 147.82 ms
interval (`ds_ark_raster_timing_first.json`). These raster times are included
in GPU command time, not additional independent categories.

The covered-row-span raster path now intersects each row with the same biased
triangle half-planes, skips uncovered interpolant steps with equivalent wrapping
32-bit arithmetic, and preserves all pixel/shading logic. All 4096 randomized
full-VRAM cases match the generic path byte-for-byte. Both targets build;
Ark/Meltigemini each pass 65000 host frames. A fresh matching DS window ending
1633 has identical call counts and drops polygon time to 55.85 ms (17.6% less),
with total interval 136.33 ms (7.8% less). Active bitmap comparison at frame
1663 reports zero changed pixels (`ark_spans_frame1663_comparison.json`,
baseline `ff9ds_ark_raster_timing`, candidate `ff9ds_ark_spans_frame1663`).
This is instrumented timing; uninstrumented combat rate and later animations
remain to be validated.

`CAPTURE_FRAME=N` makes an explicitly diagnostic DS build stop game execution
after presenting exactly that frame, while VBlank/GUI state capture remains
available. It is terminal until the emulator resets. The timing block is
published before the capture stop, so subsequent idle time must not be used
for frame-rate calculations. Ordinary builds omit this stop entirely.

Original Meltigemini was entered and visually confirmed on PSX
(`psx_meltigemini_until_current.png`). An attempt to break specifically at
DF15C produced an internal Redux event-listener exception and unresponsive
controls; that instance was closed. This does not validate morph animation
parity or establish a game/port crash.

The scaler now groups four nearest samples (five source pixels) into paired
bitmap stores when aligned and not crossing the source-row wrap. The scalar
fallback preserves odd widths, wrap and two-byte alignment. Glyph rectangles
mark coverage a word at a time rather than updating every pixel's bit. The
coverage filter and source samples are unchanged. Added tests cover paired
stores, unaligned fallback, odd widths, wrap, word boundaries and overlapping
current/previous glyph rectangles against independent nearest/full-area results.
Both target builds and display regressions pass.

At the same 128-frame Ark window ending 1633, scaler time drops from 16.12 to
9.86 ms and interval time from 136.33 to 124.97 ms after the span change.
Against the original 147.82 ms interval, the combined reduction is about 15.5%.
These are diagnostic measurements with overhead and VBlank quantization, not
an uninstrumented full-battle rate claim. Both source RGB15 and active bitmap
comparisons at frame 1663 are exact (`ark_scaler_frame1663_quality.json`).
`tools/compare_ds_display.py` provides this comparison reproducibly from two
states and matching ELFs, retaining frame numbers and viewport metadata.

Fresh uninstrumented DS Meltigemini with morph projection and covered spans
reaches frames 4056 and 6557, with zero first-fault markers and no changes in
the first 16 KiB of ITCM (`ds_meltigemini_morph_spans_current/late.ml8` and
inspection JSON, matching `ff9ds_meltigemini_morph_spans` ROM/ELF).
`ds_meltigemini_morph_spans_effects.mp4` records actual effect animation.
This validates progress past the previous native routine gap on this route,
not all morph effects, PSX phase/timing parity, victory, or the full encounter.

Opaque cached-indexed sprites and raw/neutral direct-colour sprites now have
a dedicated row path. It preserves live texture reads, CLUT feedback fallback,
transparent/STP black, texture windows, clipped UV origins and mask writes.
The 4096-case full-VRAM raster suite plus 36 focused opaque-feedback cases pass;
both target builds and 65000-frame Ark/Meltigemini host runs pass. Matching
native frame 1663 still has zero source/bitmap differences
(`ark_opaque_frame1663_quality.json`). Rectangle cost drops 23.67 to 9.99 ms;
total sampled interval is 116.61 ms versus the original 147.82 ms (about 21.1%
less). Frame-time savings differ from summed CPU phase savings because VBlank
waiting is quantized. Full uninstrumented combat-rate validation is pending.


## Return fields for direct battle fixtures (2026-10-15)

Direct battle entry now initializes the saved field ranges used by ordinary
battle cleanup. The launcher resolves field= on the selected disc, uses the
original title-overlay directory resolver to populate the area/field sectors,
and saves them in GameData.unk7EC/unk7F4 before installing the encounter ranges.
Beatrix's scripted battle exit previously restored uninitialized ranges and
issued a CD copy past the PSX image; it now returns to the configured field
and reaches the 65000-frame limit in field mode. CD wrapping was not changed.

The default return field is 70. That field is present on discs 1/2 and absent
on discs 3/4. A battle-only fixture may still run without a return field when
its route ends at Game Over. An explicit field= must be present on the selected
disc; a missing selection fails during bootstrap with a diagnostic. If a
battle fixture without valid return context actually transitions into field
mode, it stops with an explanation rather than attempting an invalid CD read.
Choose an on-disc field for fixtures intended to test escape or scripted exits.
This establishes valid loading ranges; canonical story/party/field script state
still requires appropriate event/character blobs and fixture values.

Validation artifacts include scenarios_return_context (Beatrix returns to field
mode), boss_sweep_return_bones (all 30 host fixtures pass 65000 frames), and
invalid_return_field.log (explicit missing disc-3 field 32767 rejected during
bootstrap). The earlier 10-frame negative probe ended before bootstrap and is
retained as invalid_return_field_before_bootstrap.log, not used as validation.
The separate Black Waltz entry failure was its null bone array, not a CD exit.

The catalogue-driven sweep's generated fixtures, full manifest, per-case logs
and reports stay under ignored build/validation. These are synthetic party/story
probes. Original PSX motion references currently cover selected routes, including
Valia's Holy/Freeze/Firaga and Silver Dragon's framebuffer warp; they do not
establish visual equivalence for every entry in the 30-case host sweep.
