# Audio fidelity validation

Gameplay is still silent. `port/psx/spu.c` currently implements a transfer/register
shadow, not an audible SPU. `spu_adpcm.c`, `spu_source.c`, `spu_envelope.c`, and
`spu_voice.c` are tested components for the shared sound core; they are not yet connected to
AKAO or platform audio output. Passing these tests does not establish music,
SFX, or complete SPU fidelity.

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

## Remaining sound work

Connect the mixing stage with ordered key events, direct AKAO register writes,
and the global SPU registers; implement the shared noise generator, reverb and
capture/IRQ semantics; connect PCM output on host and DS. Validate music, SFX,
movie audio, and changes of scene/bank against original PSX recordings. Test
late-game and multi-voice effects directly through the scenario harness. No
audible gameplay comparison or DS sound-output performance result is claimed yet.
