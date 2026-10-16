# Audio fidelity validation

Gameplay is still silent. `port/psx/spu.c` currently implements a transfer/register
shadow, not an audible SPU. `spu_adpcm.c`, `spu_source.c`, and `spu_envelope.c`
are tested components for the shared sound core; they are not yet connected to
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

## Remaining sound work

Connect the source/envelope components with ordered key events and direct AKAO
register writes; add Gaussian interpolation and volume-sweep register handling;
mix all 24 voices with noise/pitch modulation; implement reverb and capture/IRQ
semantics; connect PCM output on host and DS. Validate music, SFX, movie audio,
and changes of scene/bank against original PSX recordings. Test late-game and
multi-voice effects directly through the scenario harness. No audible comparison
or DS sound-output performance result is claimed yet.
