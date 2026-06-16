# microMP3 fuzzer

libFuzzer harness for the MP3 decoder.

- `fuzz_mp3_decode` drives `Mp3Decoder::decode()` with raw MP3 bytes in
  variably-sized chunks. Exercises the wrapper's frame synchronization, header
  probe, ID3v2 skipping, resync, gapless (Xing/Info/LAME) trimming, and the
  full OpenCore fixed-point bitstream decoder in one pipeline.

## Decoder configuration coverage

The harness also varies how the decoder is driven, so the chunking, equalizer,
output-buffer, and reset paths get exercised, not just a single straight-through
decode. Configuration is consumed from the tail of the input: `FuzzedDataProvider`
integral reads come off the back, so the payload prefix is preserved: only the
trailing cfg/control bytes are peeled off, leaving the front of a real `.mp3` seed
(or a merged corpus) intact. The decoder never sees the stripped tail bytes, so a
bare seed loses ~65 bytes off its end while the rest decodes as-is.

- One cfg byte (the very last byte): bit 0 replays the whole payload across a
  `reset()`; bit 1 doubles the output buffer (still valid); bit 2 periodically
  probes the undersized-output guard. Bits 3-7 are an unused mutable region.
- A run of up to 64 chunk-control bytes (cycled). Each byte drives two knobs from
  disjoint bit-fields: bits 7-3 pick the next input chunk size (1..1985 bytes,
  spanning sub-header to multi-frame), bits 2-0 pick one of the seven equalizer
  presets for the next call.
- An exhausted provider reads 0, i.e. the default: single pass, minimum output
  buffer, no undersize stress, ~257-byte chunks, flat EQ. Tiny inputs therefore
  behave predictably.

The replay bit drives the re-stream path a looping caller hits: `reset()` frees
the decoder memory and internal buffer, and the next `decode()` re-allocates and
re-probes. It adds little line coverage; its value is exercising the
free/realloc/decode ordering under ASan, which a single pass never does.

The undersize-output stress passes a buffer below `MP3_MIN_OUTPUT_BUFFER_BYTES`.
After the header is parsed this is a pure no-op (the decoder rejects the size
before touching any state), so it exercises the `MP3_OUTPUT_BUFFER_TOO_SMALL`
contract without disturbing forward progress.

On every decode the harness asserts a set of structural invariants
(single-decode, no reference needed): bytes consumed never exceed the bytes
offered; the channel count is 0 (pre-probe) or 1/2; the per-channel sample count
never exceeds `MP3_MAX_SAMPLES_PER_FRAME`; samples are only emitted once the
channel count is known; the written PCM fits the output buffer and is a whole
number of sample-frames; and on `MP3_STREAM_INFO_READY` no audio is emitted, at
most a few header bytes are consumed, and the reported sample rate is one of the
nine valid MP3 rates. A violation aborts, surfacing it like any sanitizer
finding. A forward-progress guard widens the input window when a frame needs
more contiguous bytes and bails rather than spinning.

## Requirements

- A Clang with the libFuzzer runtime.
  - **macOS:** `brew install llvm`: Apple's stock clang omits the libFuzzer
    runtime, so the Homebrew build is required.
  - **Linux:** the system `clang++` already ships libFuzzer; no extra install.
- ffmpeg with libmp3lame on `PATH` for corpus generation.

The build commands below use `$CLANGXX` for the compiler. Point it at the right
Clang for your platform:

```sh
export CLANGXX=$(brew --prefix llvm)/bin/clang++   # macOS / Homebrew LLVM
export CLANGXX=clang++                             # Linux / system clang
```

## Build

```sh
cd tests/fuzz
cmake -B build-libfuzzer -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-libfuzzer
```

For crash reproducers and the standalone torture battery (no libFuzzer runtime):

```sh
cmake -B build-standalone -DFUZZ_USE_LIBFUZZER=OFF -DCMAKE_CXX_COMPILER="$CLANGXX" .
cmake --build build-standalone
./build-standalone/fuzz_mp3_decode path/to/crashing.mp3   # reproduce a crash file
./build-standalone/fuzz_mp3_decode                        # parameterless torture battery
./build-standalone/fuzz_mp3_decode -mutate seeds_mp3/br_128k.mp3   # quick local mutation loop
```

The torture battery (no args) runs empty input, a lone `0xFF`, a truncated frame
header, all-`0xFF` sync traps at several lengths, and random blobs salted with
sync words / `ID3` / `Xing` / `Info` / `LAME`. `FUZZ_ITERATIONS` sets the random
blob and mutation counts.

## Seed corpus

```sh
./generate_seeds.sh           # creates seeds_mp3/
mkdir -p corpus_mp3
cp seeds_mp3/* corpus_mp3/
```

`generate_seeds.sh` spans MPEG1/2/2.5, mono/stereo/joint-stereo, CBR/VBR,
Xing/Info/none, with/without ID3v2, a range of bitrates and sample rates, and
several content shapes (tone/sweep/noise/silence/impulse/DC) plus very short
clips. Each generated seed gets a config tail appended (see "Decoder
configuration coverage" above): because the harness consumes its cfg/control
bytes from the back of the input, a bare `.mp3` would lose its tail to those
reads. The tail is a neutral chunk-control region plus one cfg byte, so the whole
`.mp3` survives as decoder payload while libFuzzer still has a mutable region to
flip the options. A few `cfg_*.mp3` variants pre-set cfg (replay, big buffer,
undersize stress) so those paths are seeded directly rather than found by
mutation.

`seeds_mp3/` and `corpus_mp3/` are local-only and gitignored. Regenerate seeds
any time with `./generate_seeds.sh`.

### Merging an external corpus

FFmpeg is on OSS-Fuzz and fuzzes MP3, but through its own decoder, not this
one; its corpus is still raw MP3 and the bitstream/container are identical,
so it (or any pile of `.mp3` files) can be merged as generic seed material:

```sh
# point $MP3DIR at any directory of .mp3 files
./build-libfuzzer/fuzz_mp3_decode -merge=1 -max_len=65536 corpus_mp3/ "$MP3DIR/"
```

`-merge=1` keeps only inputs that add new coverage against this harness. If the
merge encounters an input that crashes, libFuzzer writes `crash-<sha>` to the
cwd, restarts, and continues, so the merge is safe to run on a dirty corpus.

## Run

```sh
./build-libfuzzer/fuzz_mp3_decode -dict=mp3.dict corpus_mp3/
```

Useful flags: `-max_total_time=60`, `-jobs=4`, `-workers=4`, `-max_len=65536`,
`-rss_limit_mb=4096`.

## UBSan suppressions

The build disables `shift-base` and `signed-integer-overflow`. The OpenCore
fixed-point DSP left-shifts negative values in ~100 places (documented in
`src/opencore-mp3dec/CHANGES.md`) and relies on modular two's-complement int32
wraparound in its Q-format butterflies (`pvmp3_dct_6`, `pvmp3_dct_9`, the
`fxp_mac32` chains). Both are UB per the C standard but the intended behavior on
every real target, and the standalone battery trips the overflow check on the
first noise frame. Leaving them on would bury real memory-safety findings. They
are benign-by-design value computations, not memory access, so disabling them
does not weaken the coverage that matters. ASan and the rest of UBSan
(out-of-bounds, shift-by-width, use-after-free, etc.) stay active. `shift-base`
matches the host CTest suite in `tests/CMakeLists.txt`.

## Corpus coverage

To see which functions the saved corpus exercises across both the wrapper
(`src/mp3_decoder.cpp`) and the OpenCore decoder fork (`src/opencore-mp3dec/`):

```sh
./coverage.sh           # per-function report on stdout
./coverage.sh --html    # also write cov-html/ for line-by-line browsing
```

The script builds a separate `build-cov/` with clang source-based coverage
instrumentation, replays `corpus_mp3/` once via libFuzzer's `-runs=0` mode, and
renders the report with `llvm-cov`. To narrow the report to the wrapper alone,
set `IGNORE_RE='(opencore-mp3dec|tests/fuzz)'`.

## When a crash is found

1. libFuzzer drops `crash-<sha>` in the current directory.
2. Minimize: `./build-libfuzzer/fuzz_mp3_decode -minimize_crash=1 -runs=10000 crash-<sha>`.
3. Reproduce under the standalone binary for cleaner stack traces:
   `./build-standalone/fuzz_mp3_decode crash-<sha>`.
4. Keep the reproducer in `crashes/`. Crash inputs are otherwise local-only (the
   repo-wide `crash-*` gitignore pattern keeps them out of the tree); commit a
   real reproducer deliberately. Replay them after decoder changes for
   regression cover:

   ```sh
   ./build-libfuzzer/fuzz_mp3_decode -runs=0 crashes/
   ```

If a crash is a genuine OpenCore decoder defect rather than a wrapper bug, fix it
in the fork and document it in `src/opencore-mp3dec/CHANGES.md` alongside the
existing fuzzing-found fixes.
