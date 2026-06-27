# microMP3 ISO conformance

Decodes the standard ISO/IEC 11172-4 and 13818-4 Layer III conformance
bitstreams with `Mp3Decoder` and compares the PCM against the reference output
that ships with each vector, sample for sample. Verifies that the fixed-point
decoder reproduces the reference audio to the accuracy the standard defines.

## Measurements

For each vector the harness decodes the whole `.bit` stream, then compares the
interleaved S16LE output against the raw `.pcm` reference over the full overlap:

- **PSNR** = `10*log10(32767^2 / MSE)`, the mean-squared sample error as a
  signal-to-noise ratio in dB. The RMS half of the accuracy criterion.
- **max |diff|**, the largest absolute difference between a decoded and a
  reference sample, in 16-bit LSBs. The peak half.

## Gate

ISO/IEC 11172-4 defines decoder **"full accuracy"** against one signal, a
20 Hz-10 kHz sine sweep at -20 dB FS, with two bounds on the difference from the
reference:

- **RMS bound:** difference RMS below `2^-15/sqrt(12)` (~0.29 LSB at 16-bit,
  ~101 dB PSNR).
- **Peak bound:** max |diff| at most `2^-14` of full scale = **2 LSB** at 16-bit.

That sweep is not in this set. Every gated vector but one still meets both the
~101 dB RMS bound and the 2 LSB peak, landing at ~101.4-102.2 dB. The exception
is `M2L3_noise`, a full-scale noise signal, at 99.2 dB.

The gate's PSNR floor is **96 dB** (minimp3's threshold), set below `M2L3_noise`
rather than at the full-accuracy ~101 dB so that vector passes; it stays well
above the standard's "limited accuracy" tier (`2^-11/sqrt(12)`, ~4.6 LSB,
~77 dB). The peak bound is held at the full-accuracy 2 LSB for every vector.

Thresholds are overridable (`MIN_PSNR`, `MAX_DIFF` environment variables, or
`--min-psnr` / `--max-diff` on the `conformance` binary). CI runs the defaults.

## Vectors

The reference vectors are not committed here. `run_conformance.sh` fetches them
from [lieff/minimp3](https://github.com/lieff/minimp3) via a sparse,
blob-filtered checkout of just its `vectors/` directory, pinned to a fixed
commit (`MINIMP3_REF` in the script) for reproducibility. They land in
`vectors/` (gitignored). The set covers MPEG-1, MPEG-2, and MPEG-2.5 Layer III
across the sample rates, channel modes, and block types in the ISO suites, plus
minimp3's non-standard and adversarial additions.

Some vectors are decoded and reported but excluded from the gate, because they
test robustness rather than accuracy:

- `*sideinfo*`: corrupt side information; the decoder must stay alive and
  resynchronize, not match a reference.
- `l3-he_free`: free-format, rejected by design (reported `NODATA`).
- `*vbrtag-oob-read`: adversarial out-of-bounds-read regression vector
  (`NODATA`).

Two vectors that decode with a frame offset are not excluded; the decode is
correct and the harness accounts for the offset:

- `l3-he_mode` switches mono<->stereo mid-stream. The harness honors the
  decoder's `MP3_STREAM_INFO_CHANGED` signal and emits each section in its
  native channel layout, matching the reference.
- `l3-sin1k0db` (plain) has a reference pre-trimmed by two lead-in frames, and
  the plain vector carries no gapless metadata, so the decoder emits those two
  frames. The harness aligns with a +/-2-frame search and scores the
  best-aligned PSNR. Its LAME-tagged twin aligns at offset 0 once gapless
  trimming removes the same two frames.

## Running

```sh
cd tests/conformance
./run_conformance.sh
```

The script fetches the vectors (once; cached in `vectors/` afterward), builds
the harness, decodes every vector, and prints one line each plus a final gate
verdict. Each line reports the decoded/reference sizes, recoverable frame-skip
count, the offset-0 metrics, and the best-aligned metrics:

```text
VECTOR                         RESULT
l3-compl.bit    rate=48000 ch=1 dec/ch=248832 ref/ch=248832 errs=0 | PSNR@0=102.18 maxdiff@0=1 | off=0 PSNR=102.18 maxdiff=1 | PASS
...
RESULT: all 24 gated Layer III vectors >= 96 dB PSNR and <= 2 LSB peak
```

`PASS`/`FAIL` is per the gate above. `NODATA` marks a stream this decoder does
not meaningfully decode (output overlaps the reference for less than half its
samples); it is not counted as a failure.

## Diagnostics

If a gated vector drops below the threshold, `analyze` reports where the error
lives instead of scoring it. Same inputs, comparison fixed at offset 0:

```sh
./build/analyze vectors/l3-compl.bit vectors/l3-compl.pcm
```

Per channel it reports a `|diff|` histogram, mean error bucketed by signal
amplitude (error that grows with magnitude indicates the `|is|^(4/3)`
requantization path), an error-vs-signal power spectrum in bands (band-localized
error indicates the filterbank, broadband indicates requantization), and a
per-frame PSNR distribution (systematic vs sporadic). Diagnostic only; not part
of the gate.

## CI

The `conformance` job in `.github/workflows/ci.yml` runs the gate on every push.
It caches the ~3 MB vector set keyed on the pinned `MINIMP3_REF` commit (read
from the script so the key cannot drift from what gets checked out), so only a
ref bump re-fetches.

## Links

- [lieff/minimp3](https://github.com/lieff/minimp3): source of the vectors and
  the 96 dB PSNR gate this harness mirrors;
  [its `vectors/` directory](https://github.com/lieff/minimp3/tree/master/vectors)
  holds the `.bit` streams and `.pcm` references.
- [ISO/IEC 11172-4:1995](https://www.iso.org/standard/22691.html): MPEG-1
  compliance testing; defines the "full accuracy" RMS and peak bounds.
- [ISO/IEC 13818-4:2004](https://www.iso.org/standard/40092.html): MPEG-2
  conformance testing (source of the `M2L3_*` vectors).
- [Underbit: MPEG audio decoder compliance](https://www.underbit.com/resources/mpeg/audio/compliance/)
 : freely available summary of the Annex A full- and limited-accuracy bounds
  quoted above.

The ISO catalog pages above describe the standards but the documents themselves
are paywalled. The minimp3 repository is the practical, freely available source
for both the bitstreams and the references.
