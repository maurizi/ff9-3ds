# Audio fidelity validation

Host `--audio NEW.wav` and default DS builds render game audio with SPU reverb
through AKAO and the shared sample-clocked sound core. Default host runs remain
silent. DS PCM streaming is connected, but slow gameplay causes audible starvation;
movie/CD inputs and full IRQ/DMA fidelity are unfinished.
The component/reference tests and initial game captures below do not establish
complete music/SFX fidelity.

## Block decoder

`psx_spu_decode_block` converts one 16-byte block into 28 signed PCM samples,
returns the unchanged flag byte, and retains the two clipped predictor-history
samples. Low nibbles precede high nibbles. Each prediction product is arithmetically
shifted separately before summation, and each sample is saturated before feeding
back into the predictor. Block flags do not reset history; loop address and end
handling belong to the forthcoming voice sequencer.

The independent output oracle is DuckStation's unmodified `Voice::DecodeBlock`,
extracted into a private local C++ harness by `tools/spu_adpcm_reference.py`:

- [Pinned source](https://github.com/stenzek/duckstation/blob/fe809807026e49472dd8bd99ff4771d6918a5707/src/core/spu.cpp).
- Source SHA256: `7527e40ea418aee56c95615fd4b2ece34be28ceb365ac3490adf320dbfe57003`.
- The reference source and generated executable stay under ignored `build/`;
  no reference implementation is included in the port. The committed fixtures
  are numerical results for synthetic inputs, with no game or BIOS data.
- `adpcm_cases.h` specifies inputs only: all 256 header bytes, five initial
  history pairs including both extrema, and eight consecutive blocks per case.
  The 256 fixed hashes cover 10,240 blocks / 286,720 output samples, final
  history after each block, and flag-byte preservation. Hand-computed checks
  additionally distinguish nibble order, predictor rounding, and saturation.

Reproduce the reference fixture on the host (requires C++17):

```sh
curl -fsSL https://raw.githubusercontent.com/stenzek/duckstation/fe809807026e49472dd8bd99ff4771d6918a5707/src/core/spu.cpp -o build/validation/duck_spu_pinned.cpp
python3 tools/spu_adpcm_reference.py build/validation/duck_spu_pinned.cpp > build/validation/adpcm_reference.h
cmp tests/spu/adpcm_reference.h build/validation/adpcm_reference.h
make test-spu
tools/docker.sh make -j16 ds pc spu-ds
```

`test-spu` runs with ASan/UBSan. `spu-ds` builds a disc-free ARM9 ROM running the
same production decoder and fixtures; its console reports pass/fail.

Validation on 2026-10-16: host ASan/UBSan passes, and melonDS DSi execution of
`build/validation/spu_decoder_arm9.nds` reports all 10,240 blocks passing. The
matching ELF and inspected console screenshot are `spu_decoder_arm9.elf` and
`spu_decoder_arm9.png` in the same directory. Both normal game targets build.
The test emulator was closed and its previous configuration restored. This tests
ARM9 arithmetic, not physical hardware output or sound performance.

This is an emulator arithmetic comparison, not a new physical-console capture.
Inspection of [Beetle PSX's decoder](https://github.com/libretro/beetle-psx-libretro/blob/master/mednafen/psx/spu.c)
also supports separately shifted predictor terms and clipped history, but its
reserved shifts 13..15 differ from DuckStation. The port currently follows
DuckStation and [psx-spx](https://psx-spx.consoledev.net/cdromdrive/#4bit-adpcm):
reserved shifts act as 9, and filter indices 5..15 have zero coefficients.
Reserved-input behavior still needs hardware verification. Normal shift values
0..12 and reserved values are both exercised, without treating emulator agreement
as proof of every silicon edge case.

## Source fetch and loop state

`PsxSpuSource` holds the ADPCM predictor, three previous interpolation samples,
decoded block, fractional pitch counter, current block address, and loop state.
Key-on aligns SSA to 16 bytes and resets predictor/interpolation history. It
does not change the programmed SSA or LSAX. Block fetch updates LSAX on a loop
start; a block-end flag redirects only after its samples have been traversed.
End+repeat retains predictor history. End+mute reports a separate envelope
action, allowing the full core to keep fetching silent voices when required by
SPU IRQ behavior. Noise mode suppresses that mute action but not the redirect
or ENDX action. Key-off must not reset this source state.

Explicit LSAX writes have a narrow first-block exception; afterwards they
suppress automatic loop-start updates until key-on. This follows the pinned
DuckStation `WriteVoiceRegister`, `KeyOn`, and `SampleVoice` behavior. Like the
reserved ADPCM inputs, this is not newly verified physical-console behavior.

The source tests exercise every low flag combination with noise on/off, ignored
high flag bits, manual loop writes including repeated values, predictor and
interpolation history, fractional pitch carry, zero/max pitch, the final RAM
block and address wrapping, and RAM modifications becoming visible on refetch.
These are explicit state/sequence checks, not an independent whole-SPU oracle.
Whole-block decoding matches the reference's granularity; precise RAM-fetch/IRQ
timing within a block is still outside this component.

## Envelopes and ADSR

`PsxSpuEnvelope` implements sample-clocked rate arithmetic for ADSR and volume
sweeps: separately controlled direction/exponential/phase modes, the counter
bit-15 update condition, slow-rate minimum increments, all-ones rate holds,
the exponential-increase threshold and rate-dependent slowdown, and signed
saturation. The fixed reference hashes use the same pinned source above:
`tools/spu_envelope_reference.py` extracts its unmodified `VolumeEnvelope::Reset`
and `Tick` into a private harness. The input header contains no envelope logic.

All 128 rates are exercised with both hold masks, all eight mode combinations,
and seven initial levels including both extrema and the 0x6000 threshold. Cases
at rate 104 or above run for 40,000 ticks; others run for 512. The hashes cover
113,483,776 ticks, comparing level/counter/active results. These tests continue ticking saturated
envelopes deliberately to check the primitive; the eventual sweep wrapper must
honor the returned inactive state.

`PsxSpuAdsr` adds attack/decay/sustain/release/off transitions, register decoding,
key-on reset, key-off preserving the current level, and register-write counter
reset. Hand-calculated trajectories check fast attack/decay, sustain holds,
release completion, retriggering, phase changes, and repeated key-off. These
state-machine checks supplement the independent envelope arithmetic oracle;
they do not validate the future timing of key-event delivery from game code.

Validation on 2026-10-16: all three component suites pass host ASan/UBSan and
emulated ARM9 execution. The matching test ROM/ELF and inspected console capture
are `build/validation/spu_components_arm9.nds`, `.elf`, and `.png`. Both game
targets build. The test emulator was closed and its previous configuration
restored. These remain component results, not an audible gameplay comparison.

```sh
python3 tools/spu_envelope_reference.py build/validation/duck_spu_pinned.cpp > build/validation/envelope_reference.h
cmp tests/spu/envelope_reference.h build/validation/envelope_reference.h
make test-spu
tools/docker.sh make -j16 ds pc spu-ds
```

## Gaussian interpolation, volumes, and combined voices

`psx_spu_source_interpolate` uses the documented 512 hardware coefficients.
They were compared numerically with both pinned reference tables. The LE16
coefficient bytes have SHA256
`3e221a5b25de34ca9ad1ea945ad90fbe8171c5e3f7c52af15b232f9c36fbd40d`.
Counter bits 4..11 select the phase; the four most recent samples include the
three carried from the preceding block. Products are summed before the final
15-bit arithmetic shift, following both reference emulators. The documentation's
pseudocode instead shifts each product first; that can differ by a few PCM units.
Hardware confirmation of this rounding distinction remains outstanding.

The interpolation oracle extracts the original scalar expression and table from
[Beetle PSX commit 9568c66b7386fa928b433f29e3daf7b8ee0f0031](https://github.com/libretro/beetle-psx-libretro/blob/9568c66b7386fa928b433f29e3daf7b8ee0f0031/mednafen/psx/spu.c).
Source SHA256:
`f081394befd298e1d3e70a3982009ac9555853c4dfa92162fb7bbed7d3370a99`.
Its fixed hashes cover 32 input vectors for each of 256 phases; the production
test repeats these at all 28 positions and all 16 low fractional values.

`PsxSpuVolume` implements direct signed 15-bit volume and sweep register writes,
retaining the level when changing to sweep mode, resetting counters on explicit
writes, and stopping a saturated sweep. Tests enumerate all 32,768 direct-volume
values and exercise negative, exponential, held, and restarted sweeps on top of
the existing envelope oracle.

`psx_spu_mix_voices` combines all 24 voices with Gaussian interpolation, ADSR,
pitch modulation from the preceding voice's post-ADSR sample, independent stereo
volumes/sweeps, raw dry/reverb-send sums, ENDX actions, and the voice 1/3 capture
samples. It accepts an externally supplied shared noise sample. This is a mixing
stage: the global noise clock, reverb, CD/external inputs, main volume/clipping,
register bus, key-event latches, capture RAM/IRQs, and output remain separate work.
No voice is discarded to fit the DS's 16 hardware channels.

The combined oracle extracts the pinned DuckStation voice structs, register
writer, key methods, ADSR/sweep/decoder methods, and `SampleVoice` without changing
those method bodies. It substitutes Beetle's independently checked scalar
interpolation for DuckStation's SIMD method and supplies RAM/noise data directly.
Pinned DuckStation helper headers also remain local. `GeneratePendingSamples`
is a no-op because events occur at explicit sample boundaries; this oracle does
not validate the future CPU bus or key-latch scheduling.

Eight synthetic scenarios each exercise 24 voices for 2,048 samples, comparing
dry/reverb sums, capture samples, ENDX, and each voice's envelope level/phase,
source address/pitch counter, loop address, stereo volume levels, and post-ADSR
sample. They cover zero/extreme/modulated pitches, noise substitution, off-voice
fetching, envelope/volume writes, manual loop changes, retriggers, and releases.
These comparisons exposed an already-off voice edge: a subsequent end/mute action
must retain a manually written ENVX value, as the reference does. The envelope
component now preserves that value; a dedicated assertion checks it as well.

Regenerate the additional numerical fixtures (host compiler needs C++23 for the
combined reference's pinned helper headers):

```sh
curl -fsSL https://raw.githubusercontent.com/libretro/beetle-psx-libretro/9568c66b7386fa928b433f29e3daf7b8ee0f0031/mednafen/psx/spu.c -o build/validation/beetle_spu_pinned.c
curl -fsSL https://raw.githubusercontent.com/stenzek/duckstation/fe809807026e49472dd8bd99ff4771d6918a5707/src/common/types.h -o build/validation/duck_types.h
curl -fsSL https://raw.githubusercontent.com/stenzek/duckstation/fe809807026e49472dd8bd99ff4771d6918a5707/src/common/bitfield.h -o build/validation/duck_bitfield.h
curl -fsSL https://raw.githubusercontent.com/stenzek/duckstation/fe809807026e49472dd8bd99ff4771d6918a5707/src/common/bitutils.h -o build/validation/duck_bitutils.h
python3 tools/spu_interpolation_reference.py build/validation/beetle_spu_pinned.c > build/validation/interpolation_reference.h
python3 tools/spu_voice_reference.py build/validation/duck_spu_pinned.cpp build/validation/beetle_spu_pinned.c --helpers build/validation > build/validation/voice_reference.h
cmp tests/spu/interpolation_reference.h build/validation/interpolation_reference.h
cmp tests/spu/voice_reference.h build/validation/voice_reference.h
make test-spu
tools/docker.sh make -j16 ds pc spu-ds
```

Validation on 2026-10-16: all five suites pass host ASan/UBSan and emulated ARM9
execution, including all eight combined 24-voice reference scenarios. The exact
ROM/ELF and inspected console capture are `build/validation/spu_voice_mix_arm9.nds`,
`.elf`, and `.png`. Both game targets build. The test emulator was closed and its
previous configuration restored. No game data was needed for these checks.

## Register boundary, clock, and first game PCM

`spu_core.c` connects the voice stage to explicit register writes, pending KON/KOF
events, ENDX/ENVX/LSAX/current-volume readbacks, shared noise, main-volume clipping
and PCM capture buffers. Key events take effect after one sample, with key-off
before key-on when both are pending. Halfword writes replace the corresponding
pending half, including zero writes. The current dry path has no reverb return,
CD/movie input, DMA/FIFO timing, or RAM IRQ delivery.

`port/game/spu_regs.c` supplies the AKAO hardware-writer overrides, including bulk
voice setup and initialization. It preserves the game's arithmetic and register
order, while repeated writes reach the core even if the value hasn't changed.
The original decomp checkout is untouched. The first game capture was silent:
GCC had inlined original key-on stores into AKAO before link-time weakening.
The port prelude now marks these original boundaries `noipa` to preserve calls
to their replacements, including protection against cloning. The first failing
capture, debugger traces, and the corrected capture remain in ignored validation
artifacts. This is why a passing isolated mixer test wasn't sufficient evidence.

`irq.c` advances the SPU between counter-2 events at 4,233,600 ticks/sec, one
44.1 kHz sample per 96 ticks. A normal 240 Hz AKAO interval emits 183/184 samples,
not four copies of a final register snapshot at VBlank. The SPU clock continues
when the timer stops, including a stop inside its callback. The existing virtual
video clock remains 60 Hz: this timing test does not establish physical PSX/DS
wall-clock equivalence or complete bus-level key/IRQ timing.

The noise oracle extracts pinned DuckStation `UpdateNoise` unchanged; all 64
clock settings are compared for 20,000 ticks each. The production implementation
uses the equivalent XOR feedback on bits 10,11,12,15 rather than a waveform lookup.
New integration tests check fractional sample carry, pending/repeated key events,
24-voice clipping before main volume, opposite stereo gains, current-volume and
loop/envelope readbacks, mute/enable behavior, and capture half selection.
The timer test runs the actual virtual IRQ code and checks event positions
183,367,551,735, exactly 44,100 samples and 240 events over 60 virtual frames,
and continued output with a stopped or zero-target timer.

```sh
python3 tools/spu_noise_reference.py build/validation/duck_spu_pinned.cpp > build/validation/noise_reference.h
cmp tests/spu/noise_reference.h build/validation/noise_reference.h
make test-spu
tools/docker.sh make -j16 ds pc spu-core-ds
tools/docker.sh build/pc/ff9ds --frames 7000 --status 1000 --press CROSS@2500 --press UP@2900+3 --press CROSS@3000+3 --audio build/validation/NEW_opening.wav
python3 tools/battle_smoke.py --frames 35000 --audio build/validation/NEW_baku.wav
```

WAV paths must be new files; the host refuses to overwrite an existing capture.
WAV headers are finalized on normal exit/fatal exit, at 44.1 kHz stereo PCM16.
The `--audio` option explicitly enables rendering; no software mixing cost is
added to DS gameplay at that integration stage. DS playback is now connected
as described below, with its real-time budget still unresolved.

The PsyQ transfer layer also now applies `SpuRead`'s original unsigned 0x7eff0
size clamp before address arithmetic (confirmed in the original 8005703C assembly).
Previously, a negative count could wrap `transfer_addr + n` and reach `memcpy`
with an enormous size. `tools/docker.sh make test-spu-transfer` checks this on
the 32-bit ABI under ASan/UBSan, including -1/-16 counts, near-end extents, and
callbacks. The port's existing end-of-RAM clipping remains a compatibility
policy; this test does not establish original DMA wrap/FIFO semantics.

Validation on 2026-10-16:

- All eight host ASan/UBSan suites pass (`spu_clock_host_tests.log`); the ARM9
  noise/integration ROM also passes (`spu_integration_arm9.nds`, matching `.elf`
  and inspected `.png`). These are arithmetic/state tests, not DS sound output.
- Corrected 7,000-frame opening captures contain 5,145,000 stereo samples each,
  range -23,079..20,243, with first nonzero PCM at 5.210408 seconds. The `noinline`
  and final `noipa` builds produce identical PCM SHA256
  `3cd04d76ee93f54b42d18bf6ae122ae6cfb2e2e5976f419ba939b774175605a2`.
- The scripted 35,000-frame opening reaches Baku battle (mode 2/overlay 0B), then
  returns to field (mode 1/overlay 0A) with rendering enabled and disabled. Both
  exit normally; `spu_baku_integrated.wav` contains 25,725,000 stereo samples.
  This validates that execution route, not graphical or audio equivalence.
- Direct Ark/Garland/Kuja scenarios each complete 6,000 host frames with audio
  rendering and nonzero captures (`spu_disc3_*.log/.wav`). This is entry/idle
  coverage, not victory, summon, or complete battle coverage.
- Original PSX `title_screen` state was loaded after cold-boot warmup, visually
  checked (`spu_psx_title.png`), and recorded through Redux's audio sink while
  advancing 480 VBlanks. The captured `spu_psx_title_reference.wav` has 442,368
  stereo samples. Comparing a 100-window log-magnitude spectral segment with
  the port's title interval gives mean cosine 0.93098
  (`spu_title_spectral_comparison.json`). This is coarse signal correspondence,
  not a fidelity percentage, synchronized PCM match, or proof of correct reverb.
- Reference and DS emulators were closed; melonDS's previous configuration was
  restored. No game-derived audio or test ROM is committed.

## Reverb DSP and library configuration

The shared core now routes each voice's EON send through the SPU reverb RAM
network and 39-tap down/up-sampling histories. It alternates right/left channel
steps at 44.1 kHz, advances the RAM cursor after the right step, retains FIR
history on ESA writes, clips the send before processing, and applies signed
EVOL before dry+wet saturation and main gain. Master enable suppresses reverb
RAM writes while reads and tail output continue. Voice mute also mutes the send;
it does not discard a retained wet tail. Capture writes precede reverb RAM reads.
Final main-gain saturation handles the -32768 times -32768 corner.

`tools/spu_reverb_reference.py` extracts the unmodified reverb functions from
[pinned Beetle PSX](https://github.com/libretro/beetle-psx-libretro/blob/9568c66b7386fa928b433f29e3daf7b8ee0f0031/mednafen/psx/spu.c),
SHA256 `f081394befd298e1d3e70a3982009ac9555853c4dfa92162fb7bbed7d3370a99`,
into a private local harness. No reference implementation or game assets are
committed. The 32 synthetic scenarios cover 1,048,576 stereo ticks, RAM and
output hashes, ten public presets, random and extreme coefficients, aliased
addresses, base changes, wraparound, disabled writes and retained tails.
The port implementation was written separately from the reference source.

The schedule follows Beetle's alternating-channel implementation; the earlier
pinned DuckStation version processes both channels together on alternate ticks.
The [MiSTer SPU](https://github.com/MiSTer-devel/PSX_MiSTer/blob/bc1df2492494ef1fc533ba6dba4e7c4742ee9689/rtl/spu.vhd)
also alternates channels and implements the same address adder: on an 18-bit
carry it adds ESA before masking. This can differ from generic modulo by the
work-area size for unusual offsets. MiSTer's resampler is not used as an exact
FIR oracle. These comparisons do not newly prove silicon behavior or absolute
initial channel phase.

The PsyQ replacements now set the ten documented presets and ESA, reset EVOL
on mode changes, implement selective depth and echo delay/feedback updates,
and honor the clear-work-area flag. Numeric settings come from the
[public PSX-SPX examples](https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-reverb-examples).
A disposable MIPS shim called the original SDK functions inside Redux and copied
only SPU register readbacks to scratch RAM. `tools/spu_sdk_reverb_reference.py`
reproduces 58 sequential cases; the committed fixture contains synthetic inputs
and hashes. All ten modes, invalid modes, write-enable preservation, depth masks,
partial echo updates and out-of-range 32-bit arithmetic match the original
library's register results. SDK mode 0 uses all-zero coefficients; the public
example uses harmless unit offsets instead. The original assembly additionally
shows that clearing mode 0 covers 128 bytes, although ESA reserves only 16.

The internal clear follows the port's synchronous transfer policy and preserves
the caller's transfer address/callback. It does not implement original DMA/event
timing or allocation-conflict checks; the existing SPU allocator is still a stub.

Reproduce component and SDK checks:

```sh
python3 tools/spu_reverb_reference.py build/validation/beetle_spu_pinned.c > build/validation/reverb_reference.h
cmp tests/spu/reverb_reference.h build/validation/reverb_reference.h
make test-spu
tools/docker.sh make test-spu-sdk-reverb test-spu-transfer
tools/docker.sh make -j16 ds pc spu-core-ds
# In a warmed, paused Redux with title_screen available (overwrites guest state):
python3 tools/spu_sdk_reverb_reference.py > build/validation/sdk_reverb_reference.h
cmp tests/spu/sdk_reverb_reference.h build/validation/sdk_reverb_reference.h
```

Validation on 2026-10-16: nine host ASan/UBSan suites and the separate 32-bit SDK
and transfer suites pass. The ARM9 integration ROM runs the same reverb fixtures,
noise suite and mixer state checks (`spu_reverb_arm9.nds`, matching `.elf` and
inspected `.png` under `build/validation`). This remains emulated ARM9 arithmetic,
not audible DS playback. Both game targets build. Reference/DS emulators were closed and the prior
melonDS configuration restored. The opening-to-Baku route
completes 35,000 host frames with nonzero reverb PCM; Ark/Garland/Kuja entry/idle
scenarios each complete 6,000 frames. Those runs do not establish complete battle
or scene-transition sound fidelity.

A fixed-window comparison with the earlier Redux title recording gives spectral
cosine 0.94572 for dry output and 0.91417 with reverb
(`spu_reverb_title_comparison.json`). Both use the same 100 windows, 2048-sample
Hann STFT with hop 512, log1p magnitude and mean-centered frequency vectors.
This differs from the previous comparison's preprocessing; its earlier score is
not directly comparable. The wet score is lower and requires investigation;
there is no fidelity improvement claim from this coarse, unsynchronized metric.
Redux's saved configuration exposes no separate reverb-disable setting. A fresh
original-state probe and a host debugger check agree on all 32 title coefficients,
ESA=f204, EVOL=2800/2800 and control=c081
(`reverb_psx_title_registers.json`, `reverb_host_registers.log`). This rules out
a preset/depth mismatch, not a DSP, voice-timing or capture-alignment difference.
A better next comparison is synchronized capture of individual dry/send/wet stages and
register events against a second reference, rather than tuning the DSP to one
spectral score. The shorter port clip is `spu_title_port_reverb.wav`.

## Sample-indexed game journals

Host `--spu-trace NEW.bin` records SPU resets, explicit register writes (including
repeated values), RAM transfers/clears, and eight signed stage values per sample:
stereo dry mix, clipped reverb send, raw wet output and final PCM. It enables the
sample clock with or without `--audio`. The file uses explicit little-endian
records and a completion footer. Truncated records, missing footers, invalid RAM
ranges and unknown events are rejected by the inspection/replay tools. Existing
files are never overwritten. Journals contain user-derived audio/instrument data
and belong under ignored `build/`; none are committed.

The observers are compiled out of ARM9 sample/register/transfer paths. Inspection
of the production ARM9 core object confirms no trace calls/symbol references.
The writer test (`tools/docker.sh make test-spu-journal`) drives the actual core,
PsyQ transfers and journal writer with synthetic ADPCM. It checks stage PCM
against the sink's hash, sample indices of repeated writes, in-flight RAM changes,
no-overwrite behavior and malformed/truncated journal rejection. Deliberately changing one recorded
dry value and one PCM value also makes the respective strict reference checks
fail with status 1. This is separate
from the DSP arithmetic tests.

Two reference modes isolate different parts of the game-driven audio pipeline:

- `spu_voice_reference.py --journal`: the existing pinned DuckStation voice
  methods, independently compiled, process each recorded register/RAM event.
  The replay driver supplies global masks, mute, key scheduling, capture writes
  and summation. Gaussian interpolation uses the pinned Beetle scalar expression,
  already checked against both coefficient tables. This verifies the voice stage
  on real game inputs, not a separately emulated CPU bus or full SPU.
- `spu_replay_reference.py --reverb-only`: the pinned Beetle core receives the
  same register/RAM events and the journal's dry/send values at its reverb input.
  Those values are supplied inputs, not verified outputs. Only wet output and
  final PCM are independently compared in this mode. The original reverb and
  gain computations are retained. CD input is silent; IRQ notifications are
  discarded. `--require-match` makes any compared mismatch fail the command.

The default Beetle replay instead generates voices itself. Its title dry output
first differs at sample 229,778 (port 0, reference -1), before the first wet-output
difference. Its voice model includes a four-sample decode delay and a different
initial interpolation-buffer arrangement from the pinned DuckStation methods.
It also differs in envelope and gain-update details. These are real reference
model differences; neither a coarse spectral score nor changing the port to match
one emulator establishes the physical PSX behavior. Absolute source/key timing,
ADSR edge cases and synchronized original-game recordings remain open work.

Validation on 2026-10-16:

- A 1,000-frame title run records 735,000 stereo samples, 1,763 register writes
  and nine RAM events. Dry/send output matches the DuckStation methods exactly;
  isolated reverb/final output matches Beetle exactly. PCM with tracing enabled
  and disabled has the same SHA256:
  `dac4fc9239a2671f02f08cfe4e307562b4e4a16ce3d5f3c134d651014b50a08e`.
- The disc-3 Ifrit first-use probe queues command 16 / ability 51 for actor 2,
  starts effect 445, and exits normally at 6,000 host frames. Its 4,410,000 stereo
  samples also give zero differences in both reference modes. This tests an
  executed summon, not just battle entry. It does not verify its menu selection,
  PSX presentation, every SFX, or original CPU timing.
- Evidence stays under `build/validation`: `spu_title_journal.bin`,
  `spu_title_duck_voice_replay.json`, `spu_title_reverb_isolated.json`,
  `spu_title_replay_report.json`, `spu_ifrit_spell_journal.bin`,
  `spu_ifrit_duck_voice_replay.json`, `spu_ifrit_reverb_isolated.json`, and
  `spu_ifrit_spell_probe/`. Both normal game targets and all existing sound
  component/SDK suites still pass. No emulator was needed or left running.

The replay also checks pinned `spu.h` SHA256
`feee6c16a8b885f2d85aa30181de9a17c3189767d06594c70c72c084d38862d7`.
Reproduce with the pinned sources/helpers described above (new output paths):

```sh
curl -fsSL https://raw.githubusercontent.com/libretro/beetle-psx-libretro/9568c66b7386fa928b433f29e3daf7b8ee0f0031/mednafen/psx/spu.h -o build/validation/beetle_spu_pinned.h
tools/docker.sh build/pc/ff9ds --frames 1000 --status 500 --spu-trace build/validation/NEW_title.bin --audio build/validation/NEW_title.wav
python3 tools/inspect_spu_trace.py build/validation/NEW_title.bin --writes
python3 tools/spu_voice_reference.py build/validation/duck_spu_pinned.cpp build/validation/beetle_spu_pinned.c --helpers build/validation --journal build/validation/NEW_title.bin --require-match
python3 tools/spu_replay_reference.py build/validation/beetle_spu_pinned.c build/validation/beetle_spu_pinned.h build/validation/NEW_title.bin --reverb-only --require-match
tools/docker.sh python3 tools/scenario_command_probe.py tests/scenarios/disc3_ifrit_first.cfg --command 16 --ability 51 --frames 6000 --out build/validation/NEW_ifrit_probe --audio build/validation/NEW_ifrit.wav --spu-trace build/validation/NEW_ifrit.bin
```

`--stages NEW.bin` on the inspector and `--reference-stages NEW.bin` on the Beetle
replay export eight LE32 values per frame for analysis. The exported reference
PCM is observed before Beetle's frontend-specific 75% headroom attenuation;
that attenuation is retained in the private reference code but does not belong
in the port's SPU output. Replay reports include compared stage names and first
mismatch indices; a successful process alone is not a fidelity result unless
`--require-match` was supplied or every reported count was inspected.

## DS PCM output and native pitch-call fix

DS builds now enable the shared 44.1 kHz mixer and stream its interleaved PCM16
through Maxmod. The SDK `arm7_maxmod.elf` replaces `arm7_minimal.elf`; ARM7 drives
channels 4/5 while ARM9 retains AKAO, voice processing and reverb. No original
voices are stolen and no samples are converted to native DS ADPCM. Maxmod's
[streaming API](https://blocksds.skylyrac.net/maxmod/group__nds__arm9__streaming.html)
accepts the PCM; its timer 0 refill IRQ is separate from profiling timers 2/3.
The installed implementation accepts 44100 Hz despite an older header range
comment ending at 32768.

An 8,192-frame single-producer/IRQ-consumer queue feeds a 2,048-frame Maxmod
stream after 4,096 frames of prefill. A full queue blocks the foreground producer
until the IRQ frees space. An empty queue emits counted zero samples and retains
future PCM in order; it does not replay stale queue contents or discard later
samples. Cursor arithmetic handles 32-bit rollover (about 27 hours at 44.1 kHz).
Maxmod performs the final deinterleave/cache flush for ARM7. Queue/output storage
is approximately 48 KiB. `AUDIO=0` disables DS mixing/output for diagnostic
comparisons; it is not the fidelity target.

`make test-pcm-ring` runs ASan/UBSan coverage for order, stereo extrema, full queues,
partial requests, starvation/recovery and rollover. `spu-stream-ds` runs the same
queue test on ARM9, then sends distinct synthetic stereo tones through the actual
ARM7 output path, pauses the producer and forces a burst large enough to block.
In the inspected melonDS DSi run, all 191,100 submitted frames were consumed,
19,115 silent frames were counted during the deliberate pause, 13 producer waits
occurred and there were zero format errors. Artifacts are `ds_pcm_stream.nds`,
matching `.elf`, `.ml8`, `.png` and `_stats.json` under `build/validation`.
A Pulse monitor capture (`ds_pcm_tones_verified.wav`) contains both tone channels;
its dominant peaks are approximately 442.2/294.8 Hz with the test emulator limited
to 60 FPS (`ds_pcm_tones_analysis.json`). These validate output routing/stereo;
absolute wall-clock pitch at native refresh and physical DS output remain unproven.

The first full-game ARM9/host PCM comparison exposed a separate ABI bug, rather
than an output-buffer error. Three callers cast `func_8005DFE0` to `s32 (*)()` and
pass signed `AkaoChannel.unk108` to its `u8 fine` parameter. AAPCS expects the
[caller to extend narrow arguments](https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst#parameter-passing).
The ARM callee therefore used ffffffff for fine=-1 rather than ff. The original
MIPS callee explicitly executes `andi a2,a2,0xff` at 8005E0B4; its disassembly is
retained in `akao_pitch_mips_disassembly.log`. The counted port patch removes all
three incompatible casts and uses the real prototype, leaving the decomp untouched
(F25 in `decomp-requests.md`).

In the title, the bug changed channel 4 detune from fffffff5 to f516 and voice 1
pitch from 0ad3 to 3ff4. Instrument RAM was identical. Before the fix, 346,018
channel values differed in 441,000 stereo frames, first at sample 267,918, with
maximum absolute difference 4,197 (`ds_audio_before_comparison.json`).
`test-akao-pitch` and `spu-pitch-ds` extract the production function and its three
actual call expressions into separate translation units with synthetic tables.
Eleven signed fine values, including -32768/-129/-128/-1 and byte-wrap cases,
pass on both host and emulated ARM9 (`akao_pitch_abi.nds`, `.elf`, `.png`).

After the patch, all 441,000 stereo source frames through title game frame 600
match the host byte for byte: PCM SHA256
`b58ef5780406b8ba8b232e08f2ca4828c42dad6e58194a662e23763c9c1aade0`.
This compares the ARM9 mixer output before hardware playback, not the analog
speaker signal. `AUDIOHASH=1` publishes a diagnostic hash/nonzero count;
`AUDIOCAPTURE=N` keeps the first N stereo frames in an optional diagnostic buffer.
These options are off in normal builds. `compare_ds_audio.py` checks loaded code
against the matching ELF, compares every captured value, and can export a WAV.

Real-time playback remains poor. The title checkpoint generated 441,000 source
frames but inserted 1,817,928 silent frames before stopping playback. Checkpoint
builds now stop hardware audio when game time freezes, so delayed state capture
cannot inflate that count. Evidence is `ds_audio_checkpoint.nds`, `.elf`, `.ml8`,
`.png` and `_comparison.json`. Queue buffering is not a solution to the current
rendering/mixing throughput deficit. Native clock cadence, IRQ deadline behavior,
audio/video latency, long-running playback, and later scenes still need work.
The test emulator was closed and its prior configuration restored.

```sh
make test-pcm-ring
tools/docker.sh make test-akao-pitch spu-pitch-ds spu-stream-ds
tools/docker.sh make -j16 ds pc AUTORUN=1 AUDIOHASH=1 AUDIOCAPTURE=441000 CAPTURE_FRAME=600
# Run this exact ROM in melonDS and save its checkpoint with the matching ELF.
python3 tools/compare_ds_audio.py build/validation/NEW_checkpoint.ml8 build/ds/ff9ds.elf build/validation/NEW_host.wav --wav build/validation/NEW_ds_source.wav
# Restore the normal build configuration before committing:
tools/docker.sh make -j16 ds pc
```

## ARM9 DSP placement and throughput

`SPUARM=1 SPUITCM=1` is now the default. Seven shared DSP modules compile as
ARM instructions in ITCM; `spu_hot.h` becomes empty on the host. The normal DS
link now uses 17,984 ITCM bytes including the expanded sprite loops, raster/scaler
and SDK code/data. Guards
avoid calls for already-fetched sample blocks, inactive volume sweeps and held
sustain/off envelopes. Zero-envelope voices skip read-only interpolation while
still advancing decoder, pitch, loop/end and envelope state. No voices, sample
rate, reverb steps or fixed-point precision are reduced.

`PROFILE=1` now reports `spu_total_including_sink` and `audio_sink` separately.
The former includes the latter and may include queue waits; do not sum them.
The inspector accepts both the previous five-detail and new seven-detail layouts.
The PCM comparison tool checks relocated sound code in the state's CP15 ITCM
image as well as code in main RAM.

The following title results use identical frame-600 captures in melonDS DSi
mode with CPU clock enabled, source-PCM hashing/capture and profiling. Each
timing window contains 128 virtual frames ending at frame 478 in mode 0.
These are emulated hardware timer measurements, not host wall-clock FPS.

| Build | SPU including sink, ms/frame | Sink, ms/frame |
|---|---:|---:|
| Thumb/main RAM | 21.4332 | 0.3468 |
| ARM/main RAM | 19.1515 | 0.3676 |
| ARM/ITCM with guards | 15.0793 | 0.3745 |

All three match the same 441,000 stereo host frames exactly, with the title
SHA256 above. The final build's 43 sized ITCM function symbols also match the
ELF byte for byte at checkpoint; mutable SDK IRQ/thread objects are excluded.
Total title frame time remains 100.2909 ms, including 60.3510 ms of GPU command
work and 4.7387 ms of scaling. It still inserts 1,735,168 silent frames before
the checkpoint: audio starvation is not resolved. Artifacts are
`audio_budget_thumb`, `audio_budget_arm` and `audio_budget_guard` with matching
`.nds`, `.elf`, `.ml8`, `_profile.json` and `_pcm.json`; the guarded build also
has `_integrity.json`, under ignored `build/validation`.

The disc-free `spu-throughput-ds` benchmark separately renders 100 ms of PCM
with four or 24 voices, reverb writes off/on, and a hash-only sink with no queue
waits. At 24 voices, the unguarded Thumb baseline requires 58.613/60.608 ms per
735-sample interval; ARM/ITCM with guards requires 39.233/40.579 ms. The interval
represents 16.667 ms of sound, so even the faster build is not real-time under
this synthetic load. This measures complete-SPU cost, not a single optimized
kernel or game-wide audio load. Hashes remain `39a85811`/`e3bef436` for four
voices and `5fea8fe6`/`c3caf725` for 24. The benchmark labels its reverb-write
control bit as `wet`; both runs still execute the SPU's reverb processing.

Host ASan/UBSan sound suites pass after the guards. Both final ARM/ITCM ROMs
also report all checks passing: `spu_guard_components.png` covers decoder,
source, envelopes, Gaussian interpolation and mixing; `spu_guard_integration.png`
covers noise, core integration and reverb. These remain component and bounded
title comparisons, not complete original-game audio validation.

```sh
make test-spu build/tests/spu_throughput_host
build/tests/spu_throughput_host
tools/docker.sh make -j16 spu-ds spu-core-ds spu-throughput-ds
# Comparative diagnostic build; archive each exact ROM and ELF before rebuilding:
tools/docker.sh make -j16 spu-throughput-ds SPUARM=0 SPUITCM=0
# Restore the normal configuration:
tools/docker.sh make -j16 ds pc
```

## Block mixing (2026-10-17)

`psx_spu_advance` now mixes in spans of up to 64 samples. The first sample of
a call still takes the single-sample path, because key events apply after it.
After that, `psx_spu_mix_voices_block` runs each voice through the whole span
before the next voice. Pitch modulation reads the previous voice's post-ADSR
samples from a buffer and the noise sequence is precomputed, so order and values
are unchanged. Capture, reverb and output then run per sample over the span.
`span_independent` falls back to sample-by-sample mixing when any playing voice
could fetch SPU RAM that this span's capture or reverb writes change first.

Inside a span, a steady voice (no noise, no pitch modulation, no volume sweep)
runs to its next event in a tight loop: the end of its ADPCM block, its next
envelope step, or an ADSR phase change. Those counts follow exactly from its
counters, and the event sample itself takes the full per-sample path. The
step arithmetic is shared through `spu_inline.h` by the single-step functions
and the block mixer. The reverb FIR pairs its symmetric taps, and reverb and
capture RAM use halfword accesses.

Exactness: the throughput hashes are unchanged (`39a85811`, `e3bef436`,
`5fea8fe6`, `c3caf725`) and the SPU unit, journal and SDK reverb suites pass. The
host title's first 600 frames (including intro-movie audio) and 4,000 frames of
the `disc3_valia_pira` battle hash identically with the block path forced off.

`spu-throughput-ds` in melonDS, DSi, JIT, ms per 735-sample interval:

| voices | reverb writes | before | after |
|---|---|---:|---:|
| 4 | off | 13.58 | 8.29 |
| 4 | on | 14.92 | 9.27 |
| 24 | off | 39.23 | 25.16 |
| 24 | on | 40.58 | 26.14 |

In the game the guard first rejected nearly every span: FF9 parks idle voices on a
loop at SPU address 0, the CD input buffer. Without CD input that buffer only
receives zeros, so a span may proceed when the bytes it will write are already
zero. FF9 also enables the SPU IRQ bit, so its off voices keep fetching and
advancing. They have no envelope and now take the steady path too. A 1,500-frame
host title run then mixes every span in blocks. Title (3,000 frames of intro
movie audio) and `disc3_valia_pira` (4,000 frames) PCM still hash identically to
the forced per-sample path.

In-game, melonDS DSi, intro FMV, `spu_total_including_sink` per 60 Hz frame:
28.7 ms before block mixing, 23.4 ms with it, 11.5 ms with idle voices on the
steady path.

The synthetic 24-voice case is still above the 16.7 ms budget. About 5 ms of each interval is
per-sample work independent of voice count (reverb, output). melonDS's JIT
timing in this configuration charges main RAM and DTCM data accesses the same
(`build/bench/cache_probe.c`); with the JIT off, main RAM costs about 1.25x.
Its figures mostly track instruction counts and code placement.

## Remaining sound work

Resolve original-game timing/capture differences beyond the matched stage
replays, complete IRQ/DMA/capture timing and
streamed movie/CD inputs, eliminate DS playback starvation, and compare music,
SFX and scene/bank changes against synchronized original PSX recordings. Improve
bus/key timing and verify unresolved silicon edge cases. Exercise late-game
actions and multi-voice effects directly through the scenario harness; original
timing, sound-output performance and complete audio fidelity remain unproven.

## DS hardware-channel output against the host (2026-10-29)
The user's suggestion: validate audio on scenes that already run at the PS1's speed (the AKAO sequencer ticks 4
times per emulated VBlank, so a slow scene plays its music slow). `tools/ds_audio_capture.sh OUT.wav FIELD` runs a
DS-mode bench of a field fixture in melonDS with its frame limiter on and its stream moved to a null PulseAudio sink
(`ff9_emu`; melonDS's flatpak ignores PULSE_SINK, and needs SDL_AUDIODRIVER=pulseaudio to open a stream at all),
recording the sink's monitor; the melonDS configuration is restored after. `tools/compare_audio_wav.py HOST DS`
aligns it with the host build's exact output (`--audio`) for the same fixture. Of the 16 sampled fields at or under
17 ms per VBlank, 12 have music in their fixtures (d2_f1556, d2_f0661, d3_f2506, d1_f0755, d2_f1256, d3_f2305,
d4_f0352, d3_f1551, d1_f0303, d3_f1451, d1_f0652; d3_f2221, d3_f2150, d4_f2508 are silent there).
First comparison, d1_f0755 (HWSOUND/SPU7 build): the same music (envelope correlation 0.87 aligned); pitch
-0.05 semitones (0.9972: the DS's 59.83 Hz frames against 60, as the mixer is clocked per frame); level 1.31x the
host's; the DS's spectrum matches within 1 dB below 3 kHz but is brighter above it: +3 dB at 5 kHz, +5 at 6.3,
+9 at 8, +14 at 10 kHz. That is the hardware channels' missing interpolation (no Gaussian filter: nearest-neighbour
playback images the samples' spectrum upward), as real hardware would play it. The envelopes put the DS 2-4%
slower, but the game holds one emulated VBlank per DS VBlank there and the capture has no gaps: melonDS not holding
real time on this host is the likely cause, so the tempo isn't measured reliably by a wall-clock capture yet.
Next: the high-frequency excess (a pre-filter or 2x Gaussian upsampling of the samples the channels play, against the
4 MB budget), and a tempo measure in emulated time.
