# micro-mp3 - Claude Development Guide

ESP-IDF component wrapping the OpenCore MP3 decoder (MPEG 1/2/2.5 Layer III). Fixed-point C++ library, Apache 2.0.

## Project Structure

```text
src/opencore-mp3dec/     # Forked OpenCore MP3 decoder source (see CHANGES.md)
src/mp3_decoder.cpp      # Mp3Decoder C++ wrapper
cmake/                   # Build system modules
include/micro_mp3/       # Public API (mp3_decoder.h)
examples/                # ESP-IDF decode_benchmark
host_examples/           # Host tools (mp3_to_wav)
```

## Build Commands

### Host (macOS/Linux)

```bash
cd host_examples/mp3_to_wav
mkdir build && cd build && cmake .. && make
./mp3_to_wav input.mp3 output.wav
```

### ESP-IDF

```bash
cd examples/decode_benchmark
idf.py set-target esp32s3
idf.py build flash monitor
```

### PlatformIO

```bash
cd examples/decode_benchmark
pio run -e esp32-s3 -t upload -t monitor
```

## Key Architecture

- **Forked decoder** — `src/opencore-mp3dec/` is a fork of the OpenCore MP3 decoder. Changes from upstream are documented in `src/opencore-mp3dec/CHANGES.md`. The original upstream tarball is preserved at the repo root as a provenance reference. Changes can be made directly to the forked source.
- **No ADTS parsing** — MP3 has its own sync word (`0xFF 0xEx`); `Mp3Decoder` handles frame synchronization internally via `parse_mp3_frame_header()`. No separate container demuxer needed.
- **Lazy initialization** — `Mp3Decoder` allocates on first `decode()` call. Constructor always succeeds.
- **Header-only probe** — first `decode()` call returns `MP3_STREAM_INFO_READY` (2) after parsing the MP3 frame header (no audio decoded). Stream properties (`sample_rate_`, `output_channels_`, `bitrate_`) are set from the 4-byte header. Up to one frame of input is buffered internally; `bytes_consumed` reflects bytes buffered. Caller should advance input by `bytes_consumed` and set up their audio pipeline, then call `decode()` again — the first frame is decoded from the internal buffer (no audio is discarded).
- **No AAC+ equivalent** — MP3 is simpler; no SBR or Parametric Stereo handling needed.
- **ESP32 memory** — uses `heap_caps_malloc_prefer()` with Kconfig-controlled placement (`MP3_DECODER_PREFER_PSRAM`, etc.). Host builds use plain `malloc`.
- **`pvmp3_decoder.cpp` excluded** — depends on missing OSCL headers; all required functionality is available through `pvmp3_framedecoder.cpp` and friends. Excluded in `cmake/sources.cmake`.
- **No ARM assembly compiled** — `asm/*.s` excluded; the C equivalent fixed-point routines (`pv_mp3dec_fxd_op_c_equivalent.h`) are used on all platforms. No Xtensa-optimized multiply path (unlike micro-aac).

## Configuration (Kconfig)

Memory placement only — no feature flags (no AAC_PLUS/HQ_SBR/PARAMETRIC_STEREO equivalents):

- `CONFIG_MP3_DECODER_PREFER_PSRAM` — Try PSRAM first, fall back to internal RAM (default)
- `CONFIG_MP3_DECODER_PREFER_INTERNAL` — Try internal RAM first, fall back to PSRAM
- `CONFIG_MP3_DECODER_PSRAM_ONLY` — Strict PSRAM; fails if unavailable (requires `SPIRAM`)
- `CONFIG_MP3_DECODER_INTERNAL_ONLY` — Never use PSRAM

## Things to Watch Out For

- **Output buffer sizing** — MP3 frames need up to 4608 bytes (1152 samples × 2ch × 2 bytes for MPEG1 stereo). Use the constant `MP3_MIN_OUTPUT_BUFFER_BYTES` or `get_min_output_buffer_bytes()`; pass the constant, not `sizeof(pointer)`. MPEG2/2.5 produce 576 samples per channel but the buffer must always accommodate the MPEG1 worst case.
- **FreeRTOS stack** — heap-allocate PCM buffers in tasks; 4608-byte buffers on the stack risk stack overflow.
- **Fixed-point only** — C equivalent used on all platforms. No Xtensa `mulsh` optimizations (unlike micro-aac); expect lower performance on Xtensa than AAC-LC.
- **`get_bitrate()` returns kbps** — e.g., 128 for 128 kbps. The OpenCore `mp3_bitrate` table stores values in kbps and `tPVMP3DecoderExternal.bitRate` is set directly from it. May vary frame to frame for VBR streams.
- **`samples_decoded` is per-channel** — for stereo, `samples_decoded == 1152` means 1152 × 2 = 2304 total `int16_t` values in the output buffer.
- **`MP3_STREAM_INFO_READY` on first call** — always check for this return code. `samples_decoded` will be 0 on this call; actual PCM output begins on the next successful `decode()`.
- **`MP3_DECODE_ERROR` is recoverable** — advance input by `bytes_consumed` and continue; the wrapper already skipped the bad frame.
- **Zero-copy direct path** — when a complete frame is available without leftover buffered data, `decode_direct()` passes the caller's buffer pointer directly to OpenCore (no memcpy). Keep input data alive for the duration of the decode call.
