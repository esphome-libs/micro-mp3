# microMP3: Claude Development Guide

ESP-IDF component wrapping a forked OpenCore MP3 decoder (fixed-point MPEG 1/2/2.5 Layer III) with PSRAM-aware allocation, lazy init, built-in frame synchronization, gapless trimming, and selectable EQ presets. Apache 2.0.

## Documentation Map

- [README.md](README.md) - Public API, usage example, Kconfig options, EQ presets, return codes, performance and memory numbers
- [src/opencore-mp3dec/CHANGES.md](src/opencore-mp3dec/CHANGES.md) - File-by-file changelog of the OpenCore fork vs upstream

## Layout

```text
src/opencore-mp3dec/        # Forked OpenCore MP3 decoder, modified in place (no submodule, no patches)
src/mp3_decoder.cpp         # Mp3Decoder C++ wrapper (frame sync, probe, ID3v2 skip, gapless trim)
include/micro_mp3/          # Public API header (mp3_decoder.h)
cmake/                      # Build modules (source lists, host and ESP-IDF configuration)
examples/decode_benchmark/  # ESP32 benchmark example (esp32, esp32s3, esp32p4)
host_examples/mp3_to_wav/   # mp3_to_wav CLI decoder
tests/                      # Mp3Decoder unit tests (ctest; fixtures in tests/data/)
tests/fuzz/                 # libFuzzer harness (self-contained CMake project, not part of ctest)
script/clang-tidy.sh        # Lint wrapper (uses the mp3_to_wav build's compile_commands.json)
```

## Build and Test

### ESP32 (PlatformIO)

```bash
cd examples/decode_benchmark
pio run -e esp32s3                       # build (also: -e esp32, -e esp32p4)
pio run -e esp32s3 -t upload -t monitor  # flash and watch benchmark output
pio run -e esp32s3 -t menuconfig         # Component config, microMP3 Decoder
```

### ESP32 (ESP-IDF)

```bash
cd examples/decode_benchmark
idf.py set-target esp32s3
idf.py build flash monitor
```

### Host (macOS/Linux)

```bash
cd host_examples/mp3_to_wav
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
./build/mp3_to_wav input.mp3 output.wav
```

### Unit tests

```bash
cd tests
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Fixtures are checked into `tests/data/`; regenerate with `tests/generate_test_data.sh` (needs ffmpeg with libmp3lame). Most tests use the all-at-once full-buffer decode of a fixture as a self-consistency reference that streamed and modified-input decodes must match; `decode_accuracy` additionally checks recovered tone power with a Goertzel filter.

Add `-DENABLE_WERROR=ON` to the host or test cmake command to treat warnings as errors (off by default). With `ENABLE_SANITIZERS=ON` the build disables only UBSan's `shift-base` check. The OpenCore DSP intentionally left-shifts negatives in ~100 documented places; all other checks stay live.

## Working Notes

- Edit OpenCore files directly in `src/opencore-mp3dec/` - there is no staging or patching step. Record any upstream divergence in `src/opencore-mp3dec/CHANGES.md`.
- Fixed-point only, C equivalent on all platforms. No Xtensa `mulsh` path (unlike micro-aac); expect lower throughput on Xtensa than AAC-LC. Watch for integer overflow when touching the DSP.
- `Mp3Decoder` allocates lazily on the first `decode()`; the constructor always succeeds. The first `decode()` returns `MP3_STREAM_INFO_READY` (2) with `samples_decoded == 0` - read `get_sample_rate()`/`get_channels()`/`get_bitrate()`, then call again for PCM. Always check for this code.
- `MP3_DECODE_ERROR` is recoverable: advance input by `bytes_consumed` and keep going (the wrapper already skipped the bad frame). Only `result < 0` other than that is fatal.
- Output buffer must always fit the MPEG1 worst case (1152 samples x 2ch x 2 bytes = 4608). Pass `MP3_MIN_OUTPUT_BUFFER_BYTES` / `get_min_output_buffer_bytes()`, never `sizeof(pointer)`. MPEG2/2.5 emit 576 samples/channel but the buffer requirement does not shrink.
- `samples_decoded` is per channel: stereo `1152` means 2304 `int16_t` written. `get_bitrate()` is kbps and may vary per frame on VBR.
- In FreeRTOS tasks, heap-allocate PCM buffers - a 4608-byte buffer on the stack risks overflow.
- Zero-copy direct path: when a full frame is available with no buffered leftover, `decode_direct()` hands the caller's pointer straight to OpenCore (no memcpy). Keep the input alive for the duration of the call.
- Decoder state plus PSRAM-aware placement is Kconfig-controlled (`MICRO_MP3_PREFER_PSRAM` and friends; see README/Kconfig). Memory placement is the only configuration - there are no feature flags. Host builds use plain `malloc`.
