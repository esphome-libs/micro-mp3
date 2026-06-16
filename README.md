# microMP3 - Embedded MP3 Decoder Wrapper

[![CI](https://github.com/esphome-libs/micro-mp3/actions/workflows/ci.yml/badge.svg)](https://github.com/esphome-libs/micro-mp3/actions/workflows/ci.yml)
[![Component Registry](https://components.espressif.com/components/esphome/micro-mp3/badge.svg)](https://components.espressif.com/components/esphome/micro-mp3)

Streaming MP3 decoder for embedded devices. Fixed-point decoder forked from OpenCore with frame synchronization, PSRAM-aware allocation, and lazy initialization. Supports MPEG 1, 2, and 2.5 Layer III.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Features

- **Streaming decode**: Decodes directly from the caller's buffer when a complete MP3 frame is available, avoiding an intermediate copy. Falls back to internal buffering only when frames span chunk boundaries.
- **MP3 frame synchronization**: Built-in frame header parsing handles sync-word detection and frame-boundary alignment. No external demuxer needed.
- **ID3v2 tag skipping**: Automatically detects and skips ID3v2 metadata tags, even when they span chunk boundaries.
- **Gapless trimming**: Automatically removes encoder delay and end padding from files with a Xing/Info/LAME header, so output length matches the source audio.
- **PSRAM-aware allocation**: Configurable memory placement with automatic fallback.
- **MPEG version support**: MPEG 1 (44.1/48/32kHz), MPEG 2 (22.05/24/16kHz), and MPEG 2.5 (11.025/12/8kHz) Layer III
- **VBR compatible**: Bitrate reported per-frame from decoded header data
- **Probe on first frame**: Stream format (sample rate, channel count, bitrate) determined automatically from the first decoded frame before any PCM is written to the caller's buffer
- **Built-in equalizer**: 7 preset EQ modes (flat, bass boost, rock, pop, jazz, classical, talk) applied in the frequency domain. Switchable per-frame.

## Usage Example

### Embedded

```cpp
#include "micro_mp3/mp3_decoder.h"

micro_mp3::Mp3Decoder decoder;  // Constructor always succeeds (lazy init)

// Heap-allocate on embedded targets to avoid stack overflow
int16_t* pcm_buffer = new int16_t[micro_mp3::MP3_MAX_SAMPLES_PER_FRAME *
                                   micro_mp3::MP3_MAX_OUTPUT_CHANNELS];  // 4608 bytes

while (have_data) {
    size_t consumed = 0, samples = 0;
    micro_mp3::Mp3Result result = decoder.decode(
        input_ptr, input_len,
        reinterpret_cast<uint8_t*>(pcm_buffer),
        micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES,
        consumed, samples
    );

    input_ptr += consumed;
    input_len -= consumed;

    if (result == micro_mp3::MP3_STREAM_INFO_READY) {
        // Stream format parsed from header, no PCM yet
        setup_pipeline(decoder.get_sample_rate(), decoder.get_channels());
        continue;
    }
    if (result == micro_mp3::MP3_DECODE_ERROR) {
        continue;  // Skip corrupt frame, recoverable
    }
    if (result < 0) {
        break;  // Fatal error (allocation failure, invalid input, etc.)
    }

    if (samples > 0) {
        // samples is per-channel; total int16_t values = samples * channels
        process_audio(pcm_buffer, samples, decoder.get_channels());
    }
}

delete[] pcm_buffer;
```

See the [decode benchmark example](examples/decode_benchmark) for a complete working example.

### Host Tool

A standalone `mp3_to_wav` converter is included for testing on macOS/Linux:

```bash
cd host_examples/mp3_to_wav
mkdir build && cd build && cmake .. && make
./mp3_to_wav input.mp3 output.wav
```

## API Reference

### `Mp3Decoder`

| Member | Description |
| ------ | ----------- |
| `Mp3Decoder()` | Constructor, always succeeds, no allocations |
| `~Mp3Decoder()` | Destructor, frees all resources |
| `decode(input, input_len, output, output_size, bytes_consumed, samples_decoded)` | Decode one MP3 frame; see result codes below |
| `get_sample_rate()` | Sample rate in Hz (0 until first successful decode) |
| `get_channels()` | Output channel count: 1 (mono) or 2 (stereo); 0 until first decode |
| `get_bit_depth()` | Always 16 |
| `get_bytes_per_sample()` | Always 2 |
| `get_bitrate()` | Bitrate in kbps (e.g., 128); may vary frame-to-frame for VBR |
| `get_version()` | MPEG version: `MP3_MPEG1`, `MP3_MPEG2`, or `MP3_MPEG2_5` |
| `get_samples_per_frame()` | PCM samples per channel per frame (1152 for MPEG1, 576 for MPEG2/2.5; 0 until first decode) |
| `get_min_output_buffer_bytes()` | Always `MP3_MIN_OUTPUT_BUFFER_BYTES` (4608) |
| `set_equalizer(eq)` | Set equalizer preset; takes effect on next `decode()` call |
| `get_equalizer()` | Current `Mp3Equalizer` preset |
| `is_initialized()` | True once decoder memory has been allocated |
| `reset()` | Free all state; next `decode()` call re-initializes |

### Result Codes (`Mp3Result`)

| Code | Value | Meaning |
| ---- | ----- | ------- |
| `MP3_OK` | 0 | Success; check `samples_decoded` |
| `MP3_NEED_MORE_DATA` | 1 | Partial frame buffered; feed more data and call again |
| `MP3_STREAM_INFO_READY` | 2 | Stream format parsed from header; no PCM yet; advance by `bytes_consumed` |
| `MP3_INPUT_INVALID` | -1 | Null pointer or bad input |
| `MP3_ALLOCATION_FAILED` | -2 | Memory allocation failed |
| `MP3_OUTPUT_BUFFER_TOO_SMALL` | -3 | Output buffer smaller than `MP3_MIN_OUTPUT_BUFFER_BYTES` |
| `MP3_DECODE_ERROR` | -4 | Corrupt/invalid frame (recoverable; advance by `bytes_consumed`) |

Use `result < 0` to check for any error. Use `result >= 0` for non-error (success or informational).

### Constants

| Constant | Value | Description |
| -------- | ----- | ----------- |
| `MP3_MAX_SAMPLES_PER_FRAME` | 1152 | Max PCM samples per channel per frame (MPEG1) |
| `MP3_MAX_OUTPUT_CHANNELS` | 2 | Max output channels |
| `MP3_MIN_OUTPUT_BUFFER_BYTES` | 4608 | Min output buffer size (1152 x 2ch x 2 bytes) |
| `MP3_INPUT_BUFFER_SIZE` | 1536 | Internal input buffer size |

### Equalizer Presets (`Mp3Equalizer`)

| Preset | Description |
| ------ | ----------- |
| `MP3_EQ_FLAT` | No equalization (default) |
| `MP3_EQ_BASS_BOOST` | Boost low frequencies relative to high |
| `MP3_EQ_ROCK` | Bass + mid emphasis |
| `MP3_EQ_POP` | Mid-high cut |
| `MP3_EQ_JAZZ` | Low-mid emphasis |
| `MP3_EQ_CLASSICAL` | Low emphasis |
| `MP3_EQ_TALK` | Mid emphasis, high cut |

The equalizer operates on 32 subbands in the frequency domain during decode. All non-flat presets only attenuate (gains ≤ 0 dB), so they will not introduce clipping but may reduce overall volume. The preset can be changed between `decode()` calls and takes effect on the next `decode()` call:

```cpp
decoder.set_equalizer(micro_mp3::MP3_EQ_BASS_BOOST);
// ... subsequent decode() calls use bass boost
decoder.set_equalizer(micro_mp3::MP3_EQ_FLAT);
// ... back to flat
```

## Configuration

```bash
idf.py menuconfig
# Navigate to: Component config, microMP3 Decoder
```

### Memory Placement

Decoder state memory can be configured with four placement options:

| Option | Default | Description |
| ------ | ------- | ----------- |
| `CONFIG_MICRO_MP3_PREFER_PSRAM` | y | Try PSRAM first, fall back to internal RAM |
| `CONFIG_MICRO_MP3_PREFER_INTERNAL` | | Try internal RAM first, fall back to PSRAM |
| `CONFIG_MICRO_MP3_PSRAM_ONLY` | | Strict PSRAM; fails if unavailable |
| `CONFIG_MICRO_MP3_INTERNAL_ONLY` | | Never use PSRAM |

Prefer PSRAM (the default) conserves internal RAM at a slight performance cost. Prefer internal RAM for better decode throughput when RAM is plentiful.

## Performance

Decoding 48 kHz stereo MP3, single stream (128 kbps clip):

| Chip | Clock | Wall-clock (30s audio) | Per-task RTF | Throughput |
| ---- | ----- | ---------------------- | ------------ | ---------- |
| ESP32 | 240 MHz | 9.7s | 0.323 | 3.1x real-time |
| ESP32-S3 | 240 MHz | 2.5s | 0.082 | 12.1x real-time |
| ESP32-P4 | 360 MHz | 1.5s | 0.050 | 19.8x real-time |

Decoder state stays in PSRAM by default on every target (the measured footprint lands entirely in PSRAM). Even on the original ESP32, where PSRAM is much slower than internal SRAM, a single MP3 stream clears real-time. The ESP32-P4 decodes about 1.6x faster per stream than the ESP32-S3. Decode cost scales with bitrate: the 320 kbps clip runs roughly 50% slower per frame than the 64 kbps clip.

The dual-core ESP32-S3 also scales across concurrent streams, each decoder instance on its own task and core (128 kbps clip):

| Concurrent tasks | Wall-clock (30s audio) | Per-task RTF | Throughput |
| ---------------- | ---------------------- | ------------ | ---------- |
| 1 | 2.5s | 0.082 | 12.1x real-time |
| 2 (one per core) | 2.9s | 0.098 | 10.2x real-time each |
| 3 | 5.8s | 0.192 | 5.2x real-time |
| 4 | 6.9s | 0.229 | 4.4x real-time |

Each stream decodes on a single thread. Running a second stream on the other core barely raises wall-clock time (2.9s against 2.5s) while nearly doubling combined throughput, but the two cores share one PSRAM bus, so adding a third and fourth task slows every task under contention. See [examples/decode_benchmark/README.md](examples/decode_benchmark/README.md) for per-frame timing statistics across all three bitrates, the test clip details, and instructions for running your own benchmark.

## Memory Usage

| Allocation | Size | Notes |
| ---------- | ---- | ----- |
| Decoder state | ~27.3KB | Allocated via `pvmp3_decoderMemRequirements()`; PSRAM preferred by default |
| Internal input buffer | 1.5KB | MP3 frame accumulation (`MP3_INPUT_BUFFER_SIZE`, 1536 bytes) |
| PCM output buffer | 4.5KB | User-provided; `MP3_MIN_OUTPUT_BUFFER_BYTES` (4608 bytes) |

Total internal allocation: ~28.8KB (decoder state + input buffer). The PCM output buffer is caller-owned.

## Testing

The wrapper is covered by a ctest suite (`tests/`) that decodes ffmpeg-generated fixtures and checks output against self-consistent references: the stream-info probe contract, chunked streaming at adversarial chunk sizes, decode accuracy, reset and reuse, error codes, corrupt-frame recovery, leading-garbage resync, ID3v2 tag skipping, and gapless (Xing/LAME) trimming.

```bash
cd tests
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
(cd build && ctest --output-on-failure)
```

Fixtures are checked into `tests/data/`; regenerate them with `tests/generate_test_data.sh` (needs ffmpeg with libmp3lame). The host tool can also be run directly under AddressSanitizer and UBSan:

```bash
cd host_examples/mp3_to_wav
cmake -DENABLE_SANITIZERS=ON -B build && cmake --build build
./build/mp3_to_wav input.mp3 output.wav
```

Add `-DENABLE_WERROR=ON` to either cmake command to treat warnings as errors (off by default). A libFuzzer harness lives in [tests/fuzz/](tests/fuzz/) for fuzzing the streaming decoder; see its README for build and run instructions.

## License

[Apache License 2.0](LICENSE)

## Links

- [OpenCore MP3 Decoder](https://android.googlesource.com/platform/external/opencore/)
- [ISO 11172-3 (MP3 specification)](https://www.iso.org/standard/22412.html)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
