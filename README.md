# microMP3 - Embedded MP3 Decoder Wrapper

[![CI](https://github.com/esphome-libs/micro-mp3/actions/workflows/ci.yml/badge.svg)](https://github.com/esphome-libs/micro-mp3/actions/workflows/ci.yml)
[![Component Registry](https://components.espressif.com/components/esphome/micro-mp3/badge.svg)](https://components.espressif.com/components/esphome/micro-mp3)

Streaming MP3 decoder for embedded devices. Fixed-point decoder forked from OpenCore with frame synchronization, PSRAM-aware allocation, and lazy initialization. Supports MPEG 1, 2, and 2.5 Layer III.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Features

- **Streaming decode**: Decodes directly from the caller's buffer when a complete MP3 frame is available, avoiding an intermediate copy. Falls back to internal buffering only when frames span chunk boundaries.
- **MP3 frame synchronization**: Built-in frame header parsing handles sync-word detection and frame-boundary alignment. No external demuxer needed.
- **ID3v2 tag skipping**: Automatically detects and skips ID3v2 metadata tags, even when they span chunk boundaries.
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
# Navigate to: Component config → MP3 Decoder
```

### Memory Placement

Decoder state memory can be configured with four placement options:

| Option | Default | Description |
| ------ | ------- | ----------- |
| `CONFIG_MP3_DECODER_PREFER_PSRAM` | y | Try PSRAM first, fall back to internal RAM |
| `CONFIG_MP3_DECODER_PREFER_INTERNAL` | | Try internal RAM first, fall back to PSRAM |
| `CONFIG_MP3_DECODER_PSRAM_ONLY` | | Strict PSRAM; fails if unavailable |
| `CONFIG_MP3_DECODER_INTERNAL_ONLY` | | Never use PSRAM |

Prefer PSRAM (the default) conserves internal RAM at a slight performance cost. Prefer internal RAM for better decode throughput when RAM is plentiful.

## Memory Usage

| Allocation | Size | Notes |
| ---------- | ---- | ----- |
| Decoder state | ~27.3KB | Allocated via `pvmp3_decoderMemRequirements()`; PSRAM preferred by default |
| Internal input buffer | 1.5KB | MP3 frame accumulation (`MP3_INPUT_BUFFER_SIZE`, 1536 bytes) |
| PCM output buffer | 4.5KB | User-provided; `MP3_MIN_OUTPUT_BUFFER_BYTES` (4608 bytes) |

Total internal allocation: ~28.8KB (decoder state + input buffer). The PCM output buffer is caller-owned.

## License

[Apache License 2.0](LICENSE)

## Links

- [OpenCore MP3 Decoder](https://android.googlesource.com/platform/external/opencore/)
- [ISO 11172-3 (MP3 specification)](https://www.iso.org/standard/22412.html)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)
